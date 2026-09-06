#pragma once

#include <string>
#include <vector>
#include <cstdint>

// The AOA identity the goggles validate before they will open the video path.
struct GadgetIdentity {
    uint16_t    vid, pid;
    std::string manufacturer;       // iManufacturer
    std::string product;            // iProduct
    std::string serial;             // iSerialNumber
    std::string configuration;      // iConfiguration
    std::string interface_name;     // iInterface
};

// Brings up the AOA interface on a FunctionFS instance, in one of two modes:
//
//   setup()  - self-contained. Composes the whole gadget in configfs, mounts
//              FunctionFS, writes the descriptors and binds the UDC. Undone
//              completely on teardown.
//
//   attach() - cooperative. Somebody else (an init script, an existing
//              multi-function gadget) composed the gadget and mounted
//              FunctionFS; we only write the descriptors, and optionally
//              bind/unbind a named configfs gadget around the run.
//
// Either way the endpoint files and the EP0 event stream are reached the same
// way, so the capture runtime does not care which mode it is running in.
class FunctionFsGadget {
public:
    FunctionFsGadget(std::string name, std::string udc);
    ~FunctionFsGadget();

    FunctionFsGadget(const FunctionFsGadget &) = delete;
    FunctionFsGadget &operator=(const FunctionFsGadget &) = delete;

    // Override the default mount point (/dev/ffs-<name>). Self-composed mode.
    void set_mount_point(std::string path) { mount_point_ = std::move(path); }

    bool setup(const GadgetIdentity &id);
    // gadget_dir empty => leave the UDC alone; whoever composed it owns binding.
    bool attach(const std::string &ffs_path, const std::string &gadget_dir,
                const GadgetIdentity &id);
    void teardown();

    int ep0() const { return ep0_fd_; }
    // "ep1" (IN, commands) / "ep2" (OUT, video); -1 on failure.
    int open_endpoint(const char *name) const;

    // Every USB device controller the board exposes, in /sys/class/udc order.
    static std::vector<std::string> list_udcs();
    // Remove a gadget left behind by a killed run and hand the UDC (empty =>
    // none available) back to whichever other gadget is left unbound.
    static bool cleanup_stale(const std::string &name, const std::string &mount_point,
                              const std::string &udc);

private:
    bool compose(const GadgetIdentity &id);          // build the configfs tree
    bool write_descriptors(const GadgetIdentity &id);
    bool bind();
    bool release_udc();
    bool restore_udc();

    std::string name_, udc_, mount_point_, gadget_path_;
    std::string prev_gadget_;                        // whose UDC we took; restored on teardown
    int  ep0_fd_    = -1;
    bool mounted_   = false;                         // we mounted it
    bool composed_  = false;                         // we built the configfs tree
    bool bound_     = false;                         // we bound the UDC
};

// FunctionFS descriptor / string blocks for the AOA interface.
std::vector<uint8_t> ffs_descriptors();
std::vector<uint8_t> ffs_strings(const std::string &interface_name);
