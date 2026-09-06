#include "usb_gadget.h"
#include "common.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include <linux/usb/functionfs.h>

static auto CONFIGFS  = "/sys/kernel/config";
static auto USB_GADGET_DIR = "/sys/kernel/config/usb_gadget";

// Placeholders: FunctionFS renumbers the interface and the UDC's autoconfig
// assigns the real endpoint addresses.
static constexpr uint8_t  EP_IN = 0x81, EP_OUT = 0x02;   // IN = commands, OUT = video
static constexpr uint16_t LANGID = 0x0409;               // en-US

// --- tiny sysfs/configfs helpers ---------------------------------------------
static bool write_file(const std::string &path, const std::string &value) {
    const int fd = open(path.c_str(), O_WRONLY);
    if (fd < 0) return false;
    const ssize_t n = write(fd, value.data(), value.size());
    close(fd);
    return n == static_cast<ssize_t>(value.size());
}

static std::string read_file(const std::string &path) {
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return {};
    char buf[256];
    const ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return {};
    std::string s(buf, n);
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

// configfs rejects a bad attribute at write() time, so a silent failure here
// would show up much later as a gadget the goggles refuse to talk to.
static bool set_attr(const std::string &path, const std::string &value) {
    if (write_file(path, value)) return true;
    log("cannot set " + path + " = '" + value + "'");
    return false;
}

static bool make_dir(const std::string &path) {
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

// The gadget core finishes releasing a function asynchronously, so a directory
// can still be busy for a moment after the last thing referencing it is gone.
static bool remove_dir(const std::string &path) {
    for (int i = 0; i < 20; i++) {
        if (rmdir(path.c_str()) == 0 || errno == ENOENT) return true;
        usleep(50000);
    }
    log("cannot remove " + path);
    return false;
}

static std::vector<std::string> list_dir(const std::string &path) {
    std::vector<std::string> names;
    DIR *d = opendir(path.c_str());
    if (!d) return names;
    while (const dirent *e = readdir(d))
        if (e->d_name[0] != '.') names.emplace_back(e->d_name);
    closedir(d);
    return names;
}

static bool ensure_configfs() {
    if (access(USB_GADGET_DIR, F_OK) == 0) return true;
    mount("none", CONFIGFS, "configfs", 0, nullptr);
    if (access(USB_GADGET_DIR, F_OK) == 0) return true;
    log("no " + std::string(USB_GADGET_DIR) + " - is CONFIG_USB_CONFIGFS enabled?");
    return false;
}

// --- FunctionFS descriptor blocks --------------------------------------------
static void add_u32(std::vector<uint8_t> &v, const uint32_t x) {
    v.push_back(x & 0xFF); v.push_back(x >> 8); v.push_back(x >> 16); v.push_back(x >> 24);
}

static void add_endpoint(std::vector<uint8_t> &v, const uint8_t addr, const uint16_t max_packet) {
    const uint8_t d[] = {7, 5, addr, 0x02,                       // bulk
                         static_cast<uint8_t>(max_packet & 0xFF),
                         static_cast<uint8_t>(max_packet >> 8), 0};
    v.insert(v.end(), d, d + sizeof(d));
}

// One vendor-specific interface with EP1 IN (commands) and EP2 OUT (video).
static std::vector<uint8_t> interface_block(const uint16_t max_packet) {
    std::vector<uint8_t> v = {9, 4, 0, 0, 2, 0xFF, 0xFF, 0, 1};   // iInterface = 1
    add_endpoint(v, EP_IN, max_packet);
    add_endpoint(v, EP_OUT, max_packet);
    return v;
}

std::vector<uint8_t> ffs_descriptors() {
    const std::vector<uint8_t> fs = interface_block(64);
    const std::vector<uint8_t> hs = interface_block(512);

    std::vector<uint8_t> out;
    add_u32(out, FUNCTIONFS_DESCRIPTORS_MAGIC_V2);
    add_u32(out, 0);                                              // length, patched below
    add_u32(out, FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC);
    add_u32(out, 3);                                              // fs descriptor count
    add_u32(out, 3);                                              // hs descriptor count
    out.insert(out.end(), fs.begin(), fs.end());
    out.insert(out.end(), hs.begin(), hs.end());

    const uint32_t len = out.size();
    out[4] = len & 0xFF; out[5] = len >> 8; out[6] = len >> 16; out[7] = len >> 24;
    return out;
}

std::vector<uint8_t> ffs_strings(const std::string &interface_name) {
    std::vector<uint8_t> out;
    add_u32(out, FUNCTIONFS_STRINGS_MAGIC);
    add_u32(out, 0);                                              // length, patched below
    add_u32(out, 1);                                              // one string
    add_u32(out, 1);                                              // one language
    out.push_back(LANGID & 0xFF);
    out.push_back(LANGID >> 8);
    out.insert(out.end(), interface_name.begin(), interface_name.end());
    out.push_back(0);

    const uint32_t len = out.size();
    out[4] = len & 0xFF; out[5] = len >> 8; out[6] = len >> 16; out[7] = len >> 24;
    return out;
}

// --- gadget lifecycle --------------------------------------------------------
FunctionFsGadget::FunctionFsGadget(std::string name, std::string udc)
    : name_(std::move(name)), udc_(std::move(udc)) {
    mount_point_ = "/dev/ffs-" + name_;
    gadget_path_ = std::string(USB_GADGET_DIR) + "/" + name_;
}

FunctionFsGadget::~FunctionFsGadget() { teardown(); }

static std::string udc_file_of(const std::string &gadget) {
    return std::string(USB_GADGET_DIR) + "/" + gadget + "/UDC";
}

std::vector<std::string> FunctionFsGadget::list_udcs() {
    std::vector<std::string> udcs = list_dir("/sys/class/udc");
    std::sort(udcs.begin(), udcs.end());
    return udcs;
}

// Only one gadget may own a UDC. Park whoever has it -- on a stock Rockchip
// image that is the vendor gadget running adb -- and remember the name so
// teardown() can hand it back.
bool FunctionFsGadget::release_udc() {
    for (const std::string &g : list_dir(USB_GADGET_DIR)) {
        if (g == name_ || read_file(udc_file_of(g)) != udc_) continue;
        log("releasing UDC from gadget '" + g + "'");
        if (!write_file(udc_file_of(g), "\n")) {
            log("cannot unbind gadget '" + g + "' from " + udc_);
            return false;
        }
        prev_gadget_ = g;
    }
    return true;
}

// Only ever hands the UDC to the gadget release_udc() took it from. A run that
// took nothing (attach mode, or a free controller) restores nothing.
bool FunctionFsGadget::restore_udc() {
    if (prev_gadget_.empty() || udc_.empty()) return true;
    const bool ok = write_file(udc_file_of(prev_gadget_), udc_);
    log(ok ? "UDC handed back to gadget '" + prev_gadget_ + "'"
           : "could not hand the UDC back to gadget '" + prev_gadget_ + "'");
    prev_gadget_.clear();
    return ok;
}

// Build the configfs tree: device descriptor, identity strings, one
// configuration, and the FunctionFS function linked into it.
bool FunctionFsGadget::compose(const GadgetIdentity &id) {
    const std::string g = gadget_path_;
    char buf[32];

    if (!make_dir(g)) { log("cannot create " + g); return false; }
    composed_ = true;                            // from here on teardown must run

    bool ok = true;
    snprintf(buf, sizeof(buf), "0x%04x", id.vid);
    ok &= set_attr(g + "/idVendor", buf);
    snprintf(buf, sizeof(buf), "0x%04x", id.pid);
    ok &= set_attr(g + "/idProduct", buf);
    ok &= set_attr(g + "/bcdUSB",          "0x0200");   // USB 2.0
    ok &= set_attr(g + "/bcdDevice",       "0x0200");
    ok &= set_attr(g + "/bDeviceClass",    "0x00");
    ok &= set_attr(g + "/bDeviceSubClass", "0x00");
    ok &= set_attr(g + "/bDeviceProtocol", "0x00");
    ok &= set_attr(g + "/bMaxPacketSize0", "64");
    // The AOA descriptors describe a USB 2.0 device; keep a SuperSpeed-capable
    // controller from advertising a speed we ship no descriptors for.
    ok &= set_attr(g + "/max_speed",       "high-speed");

    if (!make_dir(g + "/strings/0x409")) { log("cannot create device strings"); return false; }
    ok &= set_attr(g + "/strings/0x409/manufacturer", id.manufacturer);
    ok &= set_attr(g + "/strings/0x409/product",      id.product);
    ok &= set_attr(g + "/strings/0x409/serialnumber", id.serial);

    if (!make_dir(g + "/configs/c.1") || !make_dir(g + "/configs/c.1/strings/0x409")) {
        log("cannot create configuration"); return false;
    }
    ok &= set_attr(g + "/configs/c.1/strings/0x409/configuration", id.configuration);
    ok &= set_attr(g + "/configs/c.1/bmAttributes", "0xc0");      // self-powered
    ok &= set_attr(g + "/configs/c.1/MaxPower",     "2");         // -> bMaxPower = 1
    if (!ok) { log("the AOA identity is incomplete - the goggles would reject it"); return false; }

    const std::string fn = "ffs." + name_;
    if (!make_dir(g + "/functions/" + fn)) { log("cannot create " + fn); return false; }
    if (symlink((g + "/functions/" + fn).c_str(), (g + "/configs/c.1/" + fn).c_str()) != 0
        && errno != EEXIST) {
        log("cannot link " + fn + " into the configuration");
        return false;
    }

    if (!make_dir(mount_point_)) { log("cannot create " + mount_point_); return false; }
    // The mount's source name is what pairs it with functions/ffs.<name>.
    if (mount(name_.c_str(), mount_point_.c_str(), "functionfs", 0, nullptr) != 0) {
        perror(("mount functionfs on " + mount_point_).c_str());
        return false;
    }
    mounted_ = true;
    return true;
}

// Writing the descriptors + strings to ep0 is what makes the function (and the
// ep1/ep2 files) real; the gadget cannot bind before this.
bool FunctionFsGadget::write_descriptors(const GadgetIdentity &id) {
    ep0_fd_ = open((mount_point_ + "/ep0").c_str(), O_RDWR);
    if (ep0_fd_ < 0) {
        perror(("open " + mount_point_ + "/ep0").c_str());
        return false;
    }
    const std::vector<uint8_t> desc = ffs_descriptors();
    if (write(ep0_fd_, desc.data(), desc.size()) < 0) { perror("write descriptors"); return false; }
    const std::vector<uint8_t> str = ffs_strings(id.interface_name);
    if (write(ep0_fd_, str.data(), str.size()) < 0) { perror("write strings"); return false; }
    return true;
}

bool FunctionFsGadget::bind() {
    if (udc_.empty()) { log("no UDC to bind " + gadget_path_ + " to"); return false; }
    if (!release_udc()) return false;
    if (!write_file(gadget_path_ + "/UDC", udc_)) {
        log("cannot bind " + gadget_path_ + " to UDC " + udc_);
        return false;
    }
    bound_ = true;

    // composite_bind() bumps bcdUSB to 0x0210 to advertise LPM/BOS. The AOA
    // accessory the goggles expect is a plain USB 2.0 device, so put it back;
    // the attribute is the descriptor the host is served, and stays writable
    // while bound.
    write_file(gadget_path_ + "/bcdUSB", "0x0200");
    const std::string bcd = read_file(gadget_path_ + "/bcdUSB");
    if (bcd != "0x0200") log("warning: the kernel holds bcdUSB at " + bcd);

    log("gadget '" + name_ + "' bound to " + udc_);
    return true;
}

// --- mode 1: compose everything ourselves ------------------------------------
bool FunctionFsGadget::setup(const GadgetIdentity &id) {
    if (!ensure_configfs()) return false;
    if (access(gadget_path_.c_str(), F_OK) == 0) {
        log("gadget '" + name_ + "' already exists - run with --cleanup first");
        return false;
    }
    return compose(id) && write_descriptors(id) && bind();
}

// --- mode 2: use a gadget somebody else composed -----------------------------
bool FunctionFsGadget::attach(const std::string &ffs_path, const std::string &gadget_dir,
                              const GadgetIdentity &id) {
    if (access((ffs_path + "/ep0").c_str(), F_OK) != 0) {
        log("no ep0 under " + ffs_path + " - is FunctionFS mounted there?");
        return false;
    }
    mount_point_ = ffs_path;                     // not ours: teardown will not unmount
    log("using the FunctionFS instance at " + ffs_path);

    if (!write_descriptors(id)) return false;

    if (gadget_dir.empty()) {
        log("descriptors written; the UDC is somebody else's to bind");
        return true;
    }
    if (access(gadget_dir.c_str(), F_OK) != 0) {
        log("no such gadget: " + gadget_dir);
        return false;
    }
    gadget_path_ = gadget_dir;
    return bind();
}

int FunctionFsGadget::open_endpoint(const char *name) const {
    const int fd = open((mount_point_ + "/" + name).c_str(), O_RDWR);
    if (fd < 0) return -1;
    // The controller's autoconfig assigns the real addresses and rewrites the
    // descriptor the host is served, so report what the goggles will actually
    // see rather than what we asked for.
    usb_endpoint_descriptor d{};
    if (ioctl(fd, FUNCTIONFS_ENDPOINT_DESC, &d) == 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s -> address 0x%02x, wMaxPacketSize %u",
                 name, d.bEndpointAddress, d.wMaxPacketSize);
        log(msg);
    }
    return fd;
}

void FunctionFsGadget::teardown() {
    if (!composed_ && !bound_ && ep0_fd_ < 0) return;

    if (bound_) {
        write_file(gadget_path_ + "/UDC", "\n");
        bound_ = false;
        usleep(200000);                          // let in-flight transfers retire
    }
    if (ep0_fd_ >= 0) { close(ep0_fd_); ep0_fd_ = -1; }

    if (!composed_) {                            // attached mode: nothing else is ours
        restore_udc();
        return;
    }

    if (mounted_) {
        // Lazy on purpose. A plain umount() of this mount can park the process
        // in uninterruptible sleep inside generic_shutdown_super(), waiting on
        // the superblock's writeback workqueue -- at which point not even
        // SIGKILL gets it back. MNT_DETACH returns immediately and lets the
        // kernel release the superblock once its last reference goes away.
        umount2(mount_point_.c_str(), MNT_DETACH);
        mounted_ = false;
    }
    rmdir(mount_point_.c_str());

    const std::string g = gadget_path_;
    const std::string fn = "ffs." + name_;
    unlink((g + "/configs/c.1/" + fn).c_str());
    remove_dir(g + "/configs/c.1/strings/0x409");
    remove_dir(g + "/configs/c.1");
    remove_dir(g + "/functions/" + fn);
    remove_dir(g + "/strings/0x409");
    remove_dir(g);

    composed_ = false;
    restore_udc();
}

// The killed run took its record of the previous owner with it, so guess: the
// first other gadget that is left without a controller.
static std::string guess_previous_owner(const std::string &our_name) {
    for (const std::string &g : list_dir(USB_GADGET_DIR))
        if (g != our_name && read_file(udc_file_of(g)).empty()) return g;
    return {};
}

bool FunctionFsGadget::cleanup_stale(const std::string &name, const std::string &mount_point,
                                     const std::string &udc) {
    if (!ensure_configfs()) return false;

    FunctionFsGadget stale(name, udc);
    if (!mount_point.empty()) stale.mount_point_ = mount_point;

    if (access(stale.gadget_path_.c_str(), F_OK) != 0
        && access(stale.mount_point_.c_str(), F_OK) != 0) {
        log("nothing to clean up");
        return true;
    }
    stale.composed_ = true;
    stale.mounted_  = true;                      // the unmount is harmless if it is not
    stale.bound_    = true;
    stale.prev_gadget_ = guess_previous_owner(name);
    stale.teardown();
    log("cleaned up gadget '" + name + "'");
    return true;
}
