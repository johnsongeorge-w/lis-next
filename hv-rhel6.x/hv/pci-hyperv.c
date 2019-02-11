/*
 * Copyright (c) Microsoft Corporation.
 *
 * Author:
 *   Jake Oshins <jakeo@microsoft.com>
 *
 * This driver acts as a paravirtual front-end for PCI Express root buses.
 * When a PCI Express function (either an entire device or an SR-IOV
 * Virtual Function) is being passed through to the VM, this driver exposes
 * a new bus to the guest VM.  This is modeled as a root PCI bus because
 * no bridges are being exposed to the VM.  In fact, with a "Generation 2"
 * VM within Hyper-V, there may seem to be no PCI bus at all in the VM
 * until a device as been exposed using this driver.
 *
 * Each root PCI bus has its own PCI domain, which is called "Segment" in
 * the PCI Firmware Specifications.  Thus while each device passed through
 * to the VM using this front-end will appear at "device 0", the domain will
 * be unique.  Typically, each bus will have one PCI function on it, though
 * this driver does support more than one.
 *
 * In order to map the interrupts from the device through to the guest VM,
 * this driver also implements an IRQ Domain, which handles interrupts (either
 * MSI or MSI-X) associated with the functions on the bus.  As interrupts are
 * set up, torn down, or reaffined, this driver communicates with the
 * underlying hypervisor to adjust the mappings in the I/O MMU so that each
 * interrupt will be delivered to the correct virtual processor at the right
 * vector.  This driver does not support level-triggered (line-based)
 * interrupts, and will report that the Interrupt Line register in the
 * function's configuration space is zero.
 *
 * The rest of this driver mostly maps PCI concepts onto underlying Hyper-V
 * facilities.  For instance, the configuration space of a function exposed
 * by Hyper-V is mapped into a single page of memory space, and the
 * read and write handlers for config space must be aware of this mechanism.
 * Similarly, device setup and teardown involves messages sent to and from
 * the PCI back-end driver in Hyper-V.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, GOOD TITLE or
 * NON INFRINGEMENT.  See the GNU General Public License for more
 * details.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <asm/alternative.h>
#include <linux/kallsyms.h>
#include <linux/pci.h>
#include <linux/semaphore.h>
#include <linux/msi.h>
#include <asm/cacheflush.h>
#include "include/linux/hyperv.h"
#include <lis/asm/hyperv.h>
#include "hyperv_vmbus.h"

/*
 * Protocol versions. The low word is the minor version, the high word the
 * major version.
 */

#define PCI_MAKE_VERSION(major, minor) ((u32)(((major) << 16) | (minor)))
#define PCI_MAJOR_VERSION(version) ((u32)(version) >> 16)
#define PCI_MINOR_VERSION(version) ((u32)(version) & 0xff)

#define offsetofend(TYPE, MEMBER) \
        (offsetof(TYPE, MEMBER) + sizeof(((TYPE *)0)->MEMBER))

enum pci_protocol_version_t {
	PCI_PROTOCOL_VERSION_1_1 = PCI_MAKE_VERSION(1, 1),	// Win10
	PCI_PROTOCOL_VERSION_1_2 = PCI_MAKE_VERSION(1, 2),	// RS1
};

#define CPU_AFFINITY_ALL        -1ULL

/*
 * Supported protocol versions in the order of probing - highest go
 * first.
 */
static enum pci_protocol_version_t pci_protocol_versions[] = {
	PCI_PROTOCOL_VERSION_1_2,
	PCI_PROTOCOL_VERSION_1_1,
};

/*
 * Protocol version negotiated by hv_pci_protocol_negotiation().
 */
static enum pci_protocol_version_t pci_protocol_version;

#define PCI_CONFIG_MMIO_LENGTH	0x2000
#define CFG_PAGE_OFFSET 0x1000
#define CFG_PAGE_SIZE (PCI_CONFIG_MMIO_LENGTH - CFG_PAGE_OFFSET)

#define MAX_SUPPORTED_MSI_MESSAGES 0x400

#define STATUS_REVISION_MISMATCH 0xC0000059

/*
 * Message Types
 */

enum pci_message_type {
	/*
	 * Version 1.1
	 */
	PCI_MESSAGE_BASE                = 0x42490000,
	PCI_BUS_RELATIONS               = PCI_MESSAGE_BASE + 0,
	PCI_QUERY_BUS_RELATIONS         = PCI_MESSAGE_BASE + 1,
	PCI_POWER_STATE_CHANGE          = PCI_MESSAGE_BASE + 4,
	PCI_QUERY_RESOURCE_REQUIREMENTS = PCI_MESSAGE_BASE + 5,
	PCI_QUERY_RESOURCE_RESOURCES    = PCI_MESSAGE_BASE + 6,
	PCI_BUS_D0ENTRY                 = PCI_MESSAGE_BASE + 7,
	PCI_BUS_D0EXIT                  = PCI_MESSAGE_BASE + 8,
	PCI_READ_BLOCK                  = PCI_MESSAGE_BASE + 9,
	PCI_WRITE_BLOCK                 = PCI_MESSAGE_BASE + 0xA,
	PCI_EJECT                       = PCI_MESSAGE_BASE + 0xB,
	PCI_QUERY_STOP                  = PCI_MESSAGE_BASE + 0xC,
	PCI_REENABLE                    = PCI_MESSAGE_BASE + 0xD,
	PCI_QUERY_STOP_FAILED           = PCI_MESSAGE_BASE + 0xE,
	PCI_EJECTION_COMPLETE           = PCI_MESSAGE_BASE + 0xF,
	PCI_RESOURCES_ASSIGNED          = PCI_MESSAGE_BASE + 0x10,
	PCI_RESOURCES_RELEASED          = PCI_MESSAGE_BASE + 0x11,
	PCI_INVALIDATE_BLOCK            = PCI_MESSAGE_BASE + 0x12,
	PCI_QUERY_PROTOCOL_VERSION      = PCI_MESSAGE_BASE + 0x13,
	PCI_CREATE_INTERRUPT_MESSAGE    = PCI_MESSAGE_BASE + 0x14,
	PCI_DELETE_INTERRUPT_MESSAGE    = PCI_MESSAGE_BASE + 0x15,
	PCI_RESOURCES_ASSIGNED2		= PCI_MESSAGE_BASE + 0x16,
	PCI_CREATE_INTERRUPT_MESSAGE2	= PCI_MESSAGE_BASE + 0x17,
	PCI_DELETE_INTERRUPT_MESSAGE2	= PCI_MESSAGE_BASE + 0x18, /* unused */
	PCI_MESSAGE_MAXIMUM
};

/*
 * Structures defining the virtual PCI Express protocol.
 */

union pci_version {
	struct {
		u16 minor_version;
		u16 major_version;
	} parts;
	u32 version;
} __packed;

/*
 * Function numbers are 8-bits wide on Express, as interpreted through ARI,
 * which is all this driver does.  This representation is the one used in
 * Windows, which is what is expected when sending this back and forth with
 * the Hyper-V parent partition.
 */
union win_slot_encoding {
	struct {
		u32	func:8;
		u32	reserved:24;
	} bits;
	u32 slot;
} __packed;

/*
 * Pretty much as defined in the PCI Specifications.
 */
struct pci_function_description {
	u16	v_id;	/* vendor ID */
	u16	d_id;	/* device ID */
	u8	rev;
	u8	prog_intf;
	u8	subclass;
	u8	base_class;
	u32	subsystem_id;
	union win_slot_encoding win_slot;
	u32	ser;	/* serial number */
} __packed;

/**
 * struct hv_msi_desc
 * @vector:		IDT entry
 * @delivery_mode:	As defined in Intel's Programmer's
 *			Reference Manual, Volume 3, Chapter 8.
 * @vector_count:	Number of contiguous entries in the
 *			Interrupt Descriptor Table that are
 *			occupied by this Message-Signaled
 *			Interrupt. For "MSI", as first defined
 *			in PCI 2.2, this can be between 1 and
 *			32. For "MSI-X," as first defined in PCI
 *			3.0, this must be 1, as each MSI-X table
 *			entry would have its own descriptor.
 * @reserved:		Empty space
 * @cpu_mask:		All the target virtual processors.
 */
struct hv_msi_desc {
	u8	vector;
	u8	delivery_mode;
	u16	vector_count;
	u32	reserved;
	u64	cpu_mask;
} __packed;

/**
 * struct hv_msi_desc2 - 1.2 version of hv_msi_desc
 * @vector:		IDT entry
 * @delivery_mode:	As defined in Intel's Programmer's
 *			Reference Manual, Volume 3, Chapter 8.
 * @vector_count:	Number of contiguous entries in the
 *			Interrupt Descriptor Table that are
 *			occupied by this Message-Signaled
 *			Interrupt. For "MSI", as first defined
 *			in PCI 2.2, this can be between 1 and
 *			32. For "MSI-X," as first defined in PCI
 *			3.0, this must be 1, as each MSI-X table
 *			entry would have its own descriptor.
 * @processor_count:	number of bits enabled in array.
 * @processor_array:	All the target virtual processors.
 */
struct hv_msi_desc2 {
	u8	vector;
	u8	delivery_mode;
	u16	vector_count;
	u16	processor_count;
	u16	processor_array[32];
} __packed;

/**
 * struct tran_int_desc
 * @reserved:		unused, padding
 * @vector_count:	same as in hv_msi_desc
 * @data:		This is the "data payload" value that is
 *			written by the device when it generates
 *			a message-signaled interrupt, either MSI
 *			or MSI-X.
 * @address:		This is the address to which the data
 *			payload is written on interrupt
 *			generation.
 */
struct tran_int_desc {
	u16	reserved;
	u16	vector_count;
	u32	data;
	u64	address;
} __packed;

/*
 * A generic message format for virtual PCI.
 * Specific message formats are defined later in the file.
 */

struct pci_message {
	u32 type;
} __packed;

struct pci_child_message {
	struct pci_message message_type;
	union win_slot_encoding wslot;
} __packed;

struct pci_incoming_message {
	struct vmpacket_descriptor hdr;
	struct pci_message message_type;
} __packed;

struct pci_response {
	struct vmpacket_descriptor hdr;
	s32 status;			/* negative values are failures */
} __packed;

struct pci_packet {
	void (*completion_func)(void *context, struct pci_response *resp,
				int resp_packet_size);
	void *compl_ctxt;
	struct pci_message message[0];
};

/*
 * Specific message types supporting the PCI protocol.
 */

/*
 * Version negotiation message. Sent from the guest to the host.
 * The guest is free to try different versions until the host
 * accepts the version.
 *
 * pci_version: The protocol version requested.
 * is_last_attempt: If TRUE, this is the last version guest will request.
 * reservedz: Reserved field, set to zero.
 */

struct pci_version_request {
	struct pci_message message_type;
	u32 protocol_version;
} __packed;

/*
 * Bus D0 Entry.  This is sent from the guest to the host when the virtual
 * bus (PCI Express port) is ready for action.
 */

struct pci_bus_d0_entry {
	struct pci_message message_type;
	u32 reserved;
	u64 mmio_base;
} __packed;

struct pci_bus_relations {
	struct pci_incoming_message incoming;
	u32 device_count;
	struct pci_function_description func[0];
} __packed;

struct pci_q_res_req_response {
	struct vmpacket_descriptor hdr;
	s32 status;			/* negative values are failures */
	u32 probed_bar[6];
} __packed;

struct pci_set_power {
	struct pci_message message_type;
	union win_slot_encoding wslot;
	u32 power_state;		/* In Windows terms */
	u32 reserved;
} __packed;

struct pci_set_power_response {
	struct vmpacket_descriptor hdr;
	s32 status;			/* negative values are failures */
	union win_slot_encoding wslot;
	u32 resultant_state;		/* In Windows terms */
	u32 reserved;
} __packed;

struct pci_resources_assigned {
	struct pci_message message_type;
	union win_slot_encoding wslot;
	u8 memory_range[0x14][6];	/* not used here */
	u32 msi_descriptors;
	u32 reserved[4];
} __packed;

struct pci_resources_assigned2 {
	struct pci_message message_type;
	union win_slot_encoding wslot;
	u8 memory_range[0x14][6];	/* not used here */
	u32 msi_descriptor_count;
	u8 reserved[70];
} __packed;

struct pci_create_interrupt {
	struct pci_message message_type;
	union win_slot_encoding wslot;
	struct hv_msi_desc int_desc;
} __packed;

struct pci_create_interrupt2 {
	struct pci_message message_type;
	union win_slot_encoding wslot;
	struct hv_msi_desc2 int_desc;
} __packed;

struct pci_create_int_response {
	struct pci_response response;
	u32 reserved;
	struct tran_int_desc int_desc;
} __packed;

struct pci_delete_interrupt {
	struct pci_message message_type;
	union win_slot_encoding wslot;
	struct tran_int_desc int_desc;
} __packed;

struct pci_dev_incoming {
	struct pci_incoming_message incoming;
	union win_slot_encoding wslot;
} __packed;

struct pci_eject_response {
	struct pci_message message_type;
	union win_slot_encoding wslot;
	u32 status;
} __packed;

static int pci_ring_size = (4 * PAGE_SIZE);

/*
 * Definitions or interrupt steering hypercall.
 */
#define HV_PARTITION_ID_SELF		((u64)-1)
#define HVCALL_RETARGET_INTERRUPT	0x7e

struct hv_interrupt_entry {
	u32	source;			/* 1 for MSI(-X) */
	u32	reserved1;
	u32	address;
	u32	data;
};

#define HV_VP_SET_BANK_COUNT_MAX	5 /* current implementation limit */

struct hv_vp_set {
	u64	format;			/* 0 (HvGenericSetSparse4k) */
	u64	valid_banks;
	u64	masks[HV_VP_SET_BANK_COUNT_MAX];
};

/*
 * flags for hv_device_interrupt_target.flags
 */
#define HV_DEVICE_INTERRUPT_TARGET_MULTICAST		1
#define HV_DEVICE_INTERRUPT_TARGET_PROCESSOR_SET	2

struct hv_device_interrupt_target {
	u32	vector;
	u32	flags;
	union {
		u64		 vp_mask;
		struct hv_vp_set vp_set;
	};
};

struct retarget_msi_interrupt {
	u64 partition_id;		/* use "self" */
	u64 device_id;
	struct hv_interrupt_entry int_entry;
	u64 reserved2;
	struct hv_device_interrupt_target int_target;
} __packed;

/*
 * Driver specific state.
 */

enum hv_pcibus_state {
	hv_pcibus_init = 0,
	hv_pcibus_probed,
	hv_pcibus_installed,
	hv_pcibus_removed,
	hv_pcibus_maximum
};

struct hv_pcibus_device {
	struct pci_sysdata sysdata;
	enum hv_pcibus_state state;
	atomic_t remove_lock;
	struct hv_device *hdev;
	resource_size_t low_mmio_space;
	resource_size_t high_mmio_space;
	struct resource *mem_config;
	struct resource *low_mmio_res;
	struct resource *high_mmio_res;
	struct completion *survey_event;
	struct completion remove_event;
	struct pci_bus *pci_bus;
	spinlock_t config_lock;	/* Avoid two threads writing index page */
	spinlock_t device_list_lock;	/* Protect lists below */
	void __iomem *cfg_addr;

	struct semaphore enum_sem;
	struct list_head resources_for_children;

	struct list_head children;
	struct list_head dr_list;
	/* hypercall arg, must not cross page boundary */
	struct retarget_msi_interrupt retarget_msi_interrupt_params;
	spinlock_t retarget_msi_interrupt_lock;
};

/*
 * Tracks "Device Relations" messages from the host, which must be both
 * processed in order and deferred so that they don't run in the context
 * of the incoming packet callback.
 */
struct hv_dr_work {
	struct work_struct wrk;
	struct hv_pcibus_device *bus;
};

struct hv_dr_state {
	struct list_head list_entry;
	u32 device_count;
	struct pci_function_description func[0];
};

enum hv_pcichild_state {
	hv_pcichild_init = 0,
	hv_pcichild_requirements,
	hv_pcichild_resourced,
	hv_pcichild_ejecting,
	hv_pcichild_maximum
};

enum hv_pcidev_ref_reason {
	hv_pcidev_ref_invalid = 0,
	hv_pcidev_ref_initial,
	hv_pcidev_ref_by_slot,
	hv_pcidev_ref_packet,
	hv_pcidev_ref_pnp,
	hv_pcidev_ref_childlist,
	hv_pcidev_irqdata,
	hv_pcidev_ref_max
};

struct hv_pci_dev {
	/* List protected by pci_rescan_remove_lock */
	struct list_head list_entry;
	atomic_t refs;
	enum hv_pcichild_state state;
	struct pci_function_description desc;
	bool reported_missing;
	struct hv_pcibus_device *hbus;
	struct work_struct wrk;

	/*
	 * What would be observed if one wrote 0xFFFFFFFF to a BAR and then
	 * read it back, for each of the BAR offsets within config space.
	 */
	u32 probed_bar[6];
};

struct hv_pci_compl {
	struct completion host_event;
	s32 completion_status;
};

static struct irq_desc* (*irq_to_desc_ptr)(unsigned int irq);
#define irq_to_desc irq_to_desc_ptr
static struct pci_bus* (*pci_create_root_bus_ptr)(struct device *parent, int bus,
                struct pci_ops *ops, void *sysdata, struct list_head *resources);
#define pci_create_root_bus pci_create_root_bus_ptr
static unsigned int (*set_desc_affinity_ptr)(struct irq_desc *desc, const struct cpumask *mask,
                  unsigned int *dest_id);
#define set_desc_affinity set_desc_affinity_ptr
static void (*destroy_irq_ptr)(unsigned int irq);
#define destroy_irq destroy_irq_ptr
/**
 * hv_pci_generic_compl() - Invoked for a completion packet
 * @context:		Set up by the sender of the packet.
 * @resp:		The response packet
 * @resp_packet_size:	Size in bytes of the packet
 *
 * This function is used to trigger an event and report status
 * for any message for which the completion packet contains a
 * status and nothing else.
 */
static void hv_pci_generic_compl(void *context, struct pci_response *resp,
		     int resp_packet_size)
{
	struct hv_pci_compl *comp_pkt = context;

	if (resp_packet_size >= offsetofend(struct pci_response, status))
		comp_pkt->completion_status = resp->status;
	else
		comp_pkt->completion_status = -1;

	complete(&comp_pkt->host_event);
}

static struct hv_pci_dev *get_pcichild_wslot(struct hv_pcibus_device *hbus,
						u32 wslot);
static void get_pcichild(struct hv_pci_dev *hv_pcidev,
			 enum hv_pcidev_ref_reason reason);
static void put_pcichild(struct hv_pci_dev *hv_pcidev,
			 enum hv_pcidev_ref_reason reason);

static void get_hvpcibus(struct hv_pcibus_device *hv_pcibus);
static void put_hvpcibus(struct hv_pcibus_device *hv_pcibus);

/**
 * devfn_to_wslot() - Convert from Linux PCI slot to Windows
 * @devfn:	The Linux representation of PCI slot
 *
 * Windows uses a slightly different representation of PCI slot.
 *
 * Return: The Windows representation
 */
static u32 devfn_to_wslot(int devfn)
{
	union win_slot_encoding wslot;

	wslot.slot = 0;
	wslot.bits.func = PCI_SLOT(devfn) | (PCI_FUNC(devfn) << 5);

	return wslot.slot;
}

/**
 * wslot_to_devfn() - Convert from Windows PCI slot to Linux
 * @wslot:	The Windows representation of PCI slot
 *
 * Windows uses a slightly different representation of PCI slot.
 *
 * Return: The Linux representation
 */
static int wslot_to_devfn(u32 wslot)
{
	union win_slot_encoding slot_no;

	slot_no.slot = wslot;
	return PCI_DEVFN(0, slot_no.bits.func);
}

/*
 * PCI Configuration Space for these root PCI buses is implemented as a pair
 * of pages in memory-mapped I/O space.  Writing to the first page chooses
 * the PCI function being written or read.  Once the first page has been
 * written to, the following page maps in the entire configuration space of
 * the function.
 */

/**
 * _hv_pcifront_read_config() - Internal PCI config read
 * @hpdev:	The PCI driver's representation of the device
 * @where:	Offset within config space
 * @size:	Size of the transfer
 * @val:	Pointer to the buffer receiving the data
 */
static void _hv_pcifront_read_config(struct hv_pci_dev *hpdev, int where,
				     int size, u32 *val)
{
	unsigned long flags;
	void __iomem *addr = hpdev->hbus->cfg_addr + CFG_PAGE_OFFSET + where;

	/*
	 * If the attempt is to read the IDs or the ROM BAR, simulate that.
	 */
	if (where + size <= PCI_COMMAND) {
		memcpy(val, ((u8 *)&hpdev->desc.v_id) + where, size);
	} else if (where >= PCI_CLASS_REVISION && where + size <=
		   PCI_CACHE_LINE_SIZE) {
		memcpy(val, ((u8 *)&hpdev->desc.rev) + where -
		       PCI_CLASS_REVISION, size);
	} else if (where >= PCI_SUBSYSTEM_VENDOR_ID && where + size <=
		   PCI_ROM_ADDRESS) {
		memcpy(val, (u8 *)&hpdev->desc.subsystem_id + where -
		       PCI_SUBSYSTEM_VENDOR_ID, size);
	} else if (where >= PCI_ROM_ADDRESS && where + size <=
		   PCI_CAPABILITY_LIST) {
		/* ROM BARs are unimplemented */
		*val = 0;
	} else if (where >= PCI_INTERRUPT_LINE && where + size <=
		   PCI_INTERRUPT_PIN) {
		/*
		 * Interrupt Line and Interrupt PIN are hard-wired to zero
		 * because this front-end only supports message-signaled
		 * interrupts.
		 */
		*val = 0;
	} else if (where + size <= CFG_PAGE_SIZE) {
		spin_lock_irqsave(&hpdev->hbus->config_lock, flags);
		/* Choose the function to be read. (See comment above) */
		writel(hpdev->desc.win_slot.slot, hpdev->hbus->cfg_addr);
		/* Make sure the function was chosen before we start reading. */
		mb();
		/* Read from that function's config space. */
		switch (size) {
		case 1:
			*val = readb(addr);
			break;
		case 2:
			*val = readw(addr);
			break;
		default:
			*val = readl(addr);
			break;
		}
		/*
		 * Make sure the write was done before we release the spinlock
		 * allowing consecutive reads/writes.
		 */
		mb();
		spin_unlock_irqrestore(&hpdev->hbus->config_lock, flags);
	} else {
		dev_err(&hpdev->hbus->hdev->device,
			"Attempt to read beyond a function's config space.\n");
	}
}

/**
 * _hv_pcifront_write_config() - Internal PCI config write
 * @hpdev:	The PCI driver's representation of the device
 * @where:	Offset within config space
 * @size:	Size of the transfer
 * @val:	The data being transferred
 */
static void _hv_pcifront_write_config(struct hv_pci_dev *hpdev, int where,
				      int size, u32 val)
{
	unsigned long flags;
	void __iomem *addr = hpdev->hbus->cfg_addr + CFG_PAGE_OFFSET + where;

	if (where >= PCI_SUBSYSTEM_VENDOR_ID &&
	    where + size <= PCI_CAPABILITY_LIST) {
		/* SSIDs and ROM BARs are read-only */
	} else if (where >= PCI_COMMAND && where + size <= CFG_PAGE_SIZE) {
		spin_lock_irqsave(&hpdev->hbus->config_lock, flags);
		/* Choose the function to be written. (See comment above) */
		writel(hpdev->desc.win_slot.slot, hpdev->hbus->cfg_addr);
		/* Make sure the function was chosen before we start writing. */
		wmb();
		/* Write to that function's config space. */
		switch (size) {
		case 1:
			writeb(val, addr);
			break;
		case 2:
			writew(val, addr);
			break;
		default:
			writel(val, addr);
			break;
		}
		/*
		 * Make sure the write was done before we release the spinlock
		 * allowing consecutive reads/writes.
		 */
		mb();
		spin_unlock_irqrestore(&hpdev->hbus->config_lock, flags);
	} else {
		dev_err(&hpdev->hbus->hdev->device,
			"Attempt to write beyond a function's config space.\n");
	}
}

/**
 * hv_pcifront_read_config() - Read configuration space
 * @bus: PCI Bus structure
 * @devfn: Device/function
 * @where: Offset from base
 * @size: Byte/word/dword
 * @val: Value to be read
 *
 * Return: PCIBIOS_SUCCESSFUL on success
 *	   PCIBIOS_DEVICE_NOT_FOUND on failure
 */
static int hv_pcifront_read_config(struct pci_bus *bus, unsigned int devfn,
				   int where, int size, u32 *val)
{
	struct hv_pcibus_device *hbus =
		container_of(bus->sysdata, struct hv_pcibus_device, sysdata);
	struct hv_pci_dev *hpdev;

	hpdev = get_pcichild_wslot(hbus, devfn_to_wslot(devfn));
	if (!hpdev)
		return PCIBIOS_DEVICE_NOT_FOUND;

	_hv_pcifront_read_config(hpdev, where, size, val);

	put_pcichild(hpdev, hv_pcidev_ref_by_slot);
	return PCIBIOS_SUCCESSFUL;
}

/**
 * hv_pcifront_write_config() - Write configuration space
 * @bus: PCI Bus structure
 * @devfn: Device/function
 * @where: Offset from base
 * @size: Byte/word/dword
 * @val: Value to be written to device
 *
 * Return: PCIBIOS_SUCCESSFUL on success
 *	   PCIBIOS_DEVICE_NOT_FOUND on failure
 */
static int hv_pcifront_write_config(struct pci_bus *bus, unsigned int devfn,
				    int where, int size, u32 val)
{
	struct hv_pcibus_device *hbus =
	    container_of(bus->sysdata, struct hv_pcibus_device, sysdata);
	struct hv_pci_dev *hpdev;

	hpdev = get_pcichild_wslot(hbus, devfn_to_wslot(devfn));
	if (!hpdev)
		return PCIBIOS_DEVICE_NOT_FOUND;

	_hv_pcifront_write_config(hpdev, where, size, val);

	put_pcichild(hpdev, hv_pcidev_ref_by_slot);
	return PCIBIOS_SUCCESSFUL;
}

/* PCIe operations */
static struct pci_ops hv_pcifront_ops = {
	.read  = hv_pcifront_read_config,
	.write = hv_pcifront_write_config,
};

static inline struct pci_dev *msi_desc_to_pci_dev(struct msi_desc *desc)
{
        return desc->dev;
}

/* Interrupt management hooks */
static int hv_set_affinity(unsigned int irq, const struct cpumask *dest)
{
	struct msi_desc *msi_desc;
	struct irq_desc *desc;
	struct irq_cfg *cfg;
	struct retarget_msi_interrupt *params;
	struct hv_pcibus_device *hbus;
	struct pci_bus *pbus;
	struct pci_dev *pdev;
	int cpu, ret, cpu_vmbus;
	unsigned int dest_id;
	unsigned long flags;
	u64 res;
	u32 var_size = 0;

	msi_desc = get_irq_msi(irq);
	desc = irq_to_desc(irq);
	cfg = desc->chip_data;

	ret = set_desc_affinity(desc, dest, &dest_id);
	if (ret)
		return ret;

	pdev = msi_desc_to_pci_dev(desc->msi_desc);
	pbus = pdev->bus;
	hbus = container_of(pbus->sysdata, struct hv_pcibus_device, sysdata);

	spin_lock_irqsave(&hbus->retarget_msi_interrupt_lock, flags);

	params = &hbus->retarget_msi_interrupt_params;
	memset(params, 0, sizeof(*params));
	params->partition_id = HV_PARTITION_ID_SELF;
	params->int_entry.source = 1; /* MSI(-X) */
	params->int_entry.address = msi_desc->msg.address_lo;
	params->int_entry.data = msi_desc->msg.data;
	params->device_id = (hbus->hdev->dev_instance.b[5] << 24) |
				(hbus->hdev->dev_instance.b[4] << 16) |
				(hbus->hdev->dev_instance.b[7] << 8) |
				(hbus->hdev->dev_instance.b[6] & 0xf8) |
				PCI_FUNC(pdev->devfn);
	params->int_target.vector = cfg->vector;

	/*
	 * Honoring apic->irq_delivery_mode set to dest_Fixed by
	 * setting the HV_DEVICE_INTERRUPT_TARGET_MULTICAST flag results in a
	 * spurious interrupt storm. Not doing so does not seem to have a
	 * negative effect (yet?).
	 */

	if (pci_protocol_version >= PCI_PROTOCOL_VERSION_1_2) {
		/*
		 * PCI_PROTOCOL_VERSION_1_2 supports the VP_SET version of the
		 * HVCALL_RETARGET_INTERRUPT hypercall, which also coincides
		 * with >64 VP support.
		 * ms_hyperv.hints & HV_X64_EX_PROCESSOR_MASKS_RECOMMENDED
		 * is not sufficient for this hypercall.
		 */
		params->int_target.flags |=
			HV_DEVICE_INTERRUPT_TARGET_PROCESSOR_SET;
		params->int_target.vp_set.valid_banks =
			(1ull << HV_VP_SET_BANK_COUNT_MAX) - 1;

		/*
		 * var-sized hypercall, var-size starts after vp_mask (thus
		 * vp_set.format does not count, but vp_set.valid_banks does).
		 */
		var_size = 1 + HV_VP_SET_BANK_COUNT_MAX;
		for_each_cpu_and(cpu, dest, cpu_online_mask) {
			cpu_vmbus = vmbus_cpu_number_to_vp_number(cpu);
			if (cpu_vmbus >= HV_VP_SET_BANK_COUNT_MAX * 64) {
				dev_err(&hbus->hdev->device,
					"too high CPU %d", cpu_vmbus);
				res = 1;
				goto exit_unlock;
			}

			params->int_target.vp_set.masks[cpu_vmbus / 64] |=
				(1ULL << (cpu_vmbus & 63));
		}
	} else {
		for_each_cpu_and(cpu, dest, cpu_online_mask) {
			params->int_target.vp_mask |=
				(1ULL << vmbus_cpu_number_to_vp_number(cpu));
		}
	}

	res = hv_do_hypercall(HVCALL_RETARGET_INTERRUPT | (var_size << 17),
			      params, NULL);

exit_unlock:
	spin_unlock_irqrestore(&hbus->retarget_msi_interrupt_lock, flags);

	if (res) {
		dev_err(&hbus->hdev->device,
			"hv_irq_unmask() failed: %#llx", res);
		return -1;
	}

	return 0;
}

int hv_setup_msi_irqs(struct pci_dev *pdev, int nvec, int type)
{
	struct msi_desc *msidesc;
	struct irq_chip *chip;

	/*
	 * Call the base function which will do everything related to setting up
	 * the tracking structures.
	 */

	list_for_each_entry(msidesc, &pdev->msi_list, list) {
		if (msidesc->irq) {
			chip = get_irq_chip(msidesc->irq);
			/*
			 * Replace the affinity callback so that it doesn't
			 * rearrage the message in the hardware.
			 */
			chip->set_affinity = hv_set_affinity;
		}
	}

	return 0;
}

struct compose_comp_ctxt {
	struct hv_pci_compl comp_pkt;
	struct tran_int_desc int_desc;
};

static void hv_pci_compose_compl(void *context, struct pci_response *resp,
				 int resp_packet_size)
{
	struct compose_comp_ctxt *comp_pkt = context;
	struct pci_create_int_response *int_resp =
		(struct pci_create_int_response *)resp;

	comp_pkt->comp_pkt.completion_status = resp->status;
	comp_pkt->int_desc = int_resp->int_desc;
	complete(&comp_pkt->comp_pkt.host_event);
}

static u32 hv_compose_msi_req_v1(
	struct pci_create_interrupt *int_pkt, struct cpumask *affinity,
	u32 slot, u8 vector)
{
	int_pkt->message_type.type = PCI_CREATE_INTERRUPT_MESSAGE;
	int_pkt->wslot.slot = slot;
	int_pkt->int_desc.vector = vector;
	int_pkt->int_desc.vector_count = 1;
	int_pkt->int_desc.delivery_mode =
		(apic->irq_delivery_mode == dest_LowestPrio) ? 1 : 0;

	/*
	 * Create MSI w/ dummy vCPU set, overwritten by subsequent retarget in
	 * hv_irq_unmask().
	 */
	int_pkt->int_desc.cpu_mask = CPU_AFFINITY_ALL;

	return sizeof(*int_pkt);
}

static u32 hv_compose_msi_req_v2(
	struct pci_create_interrupt2 *int_pkt, struct cpumask *affinity,
	u32 slot, u8 vector)
{
	int cpu;

	int_pkt->message_type.type = PCI_CREATE_INTERRUPT_MESSAGE2;
	int_pkt->wslot.slot = slot;
	int_pkt->int_desc.vector = vector;
	int_pkt->int_desc.vector_count = 1;
	int_pkt->int_desc.delivery_mode =
		(apic->irq_delivery_mode == dest_LowestPrio) ? 1 : 0;

	/*
	 * Create MSI targeting just one vCPU, overwritten by subsequent
	 * retarget in hv_irq_unmask().
	 */
	cpu = cpumask_first_and(affinity, cpu_online_mask);
	int_pkt->int_desc.processor_array[0] =
		vmbus_cpu_number_to_vp_number(cpu);
	int_pkt->int_desc.processor_count = 1;

	return sizeof(*int_pkt);
}

/**
 * hv_compose_msi_msg() - Supplies a valid MSI address/data
 *
 * This function unpacks the IRQ looking for target CPU set, IDT
 * vector and mode and sends a message to the parent partition
 * asking for a mapping for that tuple in this partition.  The
 * response supplies a data value and address to which that data
 * should be written to trigger that interrupt.
 */
static int hv_compose_msi_msg(struct pci_dev *pdev, unsigned int irq,
			      struct msi_msg *msg, u8 hpet_id)
{
	struct irq_cfg *cfg = get_irq_chip_data(irq);
	struct cpumask *affinity = cfg->domain;
	struct hv_pcibus_device *hbus;
	struct hv_pci_dev *hpdev;
	struct pci_bus *pbus;
	struct compose_comp_ctxt comp;
	struct tran_int_desc *int_desc;
	struct {
		struct pci_packet pci_pkt;
		union {
			struct pci_create_interrupt v1;
			struct pci_create_interrupt2 v2;
		} int_pkts;
	} __packed ctxt;

	u32 size;
	int ret;

	pbus = pdev->bus;
	hbus = container_of(pbus->sysdata, struct hv_pcibus_device, sysdata);
	hpdev = get_pcichild_wslot(hbus, devfn_to_wslot(pdev->devfn));
	if (!hpdev)
		goto return_null_message;

	int_desc = kzalloc(sizeof(*int_desc), GFP_KERNEL);
	if (!int_desc)
		goto drop_reference;

	memset(&ctxt, 0, sizeof(ctxt));
	init_completion(&comp.comp_pkt.host_event);
	ctxt.pci_pkt.completion_func = hv_pci_compose_compl;
	ctxt.pci_pkt.compl_ctxt = &comp;

	switch (pci_protocol_version) {
	case PCI_PROTOCOL_VERSION_1_1:
		size = hv_compose_msi_req_v1(
			&ctxt.int_pkts.v1, affinity,
			hpdev->desc.win_slot.slot, cfg->vector);
		break;
	case PCI_PROTOCOL_VERSION_1_2:
		size = hv_compose_msi_req_v2(
			&ctxt.int_pkts.v2, affinity,
			hpdev->desc.win_slot.slot, cfg->vector);
		break;
	default:
		/* As we only negotiate protocol versions known to this driver,
		 * this path should never hit. However, this is it not a hot
		 * path so we print a message to aid future updates.
		 */
		dev_err(&hbus->hdev->device,
			"Unexpected vPCI protocol, update driver.");
		goto free_int_desc;
	}

	ret = vmbus_sendpacket(hpdev->hbus->hdev->channel, &ctxt.int_pkts,
			       size, (unsigned long)&ctxt.pci_pkt,
			       VM_PKT_DATA_INBAND,
			       VMBUS_DATA_PACKET_FLAG_COMPLETION_REQUESTED);
	if (ret) {
		dev_err(&hbus->hdev->device,
			"Sending request for interrupt failed: 0x%x",
			comp.comp_pkt.completion_status);
		goto free_int_desc;
	}

	wait_for_completion(&comp.comp_pkt.host_event);

	*int_desc = comp.int_desc;

	/* Pass up the result. */
	msg->address_hi = comp.int_desc.address >> 32;
	msg->address_lo = comp.int_desc.address & 0xffffffff;
	msg->data = comp.int_desc.data;

	put_pcichild(hpdev, hv_pcidev_ref_by_slot);
	return 0;

free_int_desc:
	kfree(int_desc);
drop_reference:
	put_pcichild(hpdev, hv_pcidev_ref_by_slot);
return_null_message:
	msg->address_hi = 0;
	msg->address_lo = 0;
	msg->data = 0;
	return -EFAULT;
}

void hv_teardown_msi_irqs(struct pci_dev *pdev)
{
	struct hv_pci_dev *hpdev = NULL;
	struct pci_delete_interrupt *int_pkt;
	struct msi_desc *msidesc, *entry;
	struct {
		struct pci_packet pkt;
		u8 buffer[sizeof(struct pci_delete_interrupt)];
	} ctxt;
	struct hv_pcibus_device *hbus =
		container_of(pdev->bus->sysdata, struct hv_pcibus_device,
			     sysdata);

	hpdev = get_pcichild_wslot(hbus, devfn_to_wslot(pdev->devfn));
	if (!hpdev)
		goto msi_teardown;

	list_for_each_entry(msidesc, &pdev->msi_list, list) {
		memset(&ctxt, 0, sizeof(ctxt));
		int_pkt = (struct pci_delete_interrupt *)&ctxt.pkt.message;
		int_pkt->message_type.type =
			PCI_DELETE_INTERRUPT_MESSAGE;
		int_pkt->wslot.slot = hpdev->desc.win_slot.slot;
		int_pkt->int_desc.address = (u64)msidesc->msg.address_hi << 32 |
					    msidesc->msg.address_lo;
		int_pkt->int_desc.data = msidesc->msg.data;
		vmbus_sendpacket(hbus->hdev->channel, int_pkt, sizeof(*int_pkt),
				 (unsigned long)&ctxt.pkt, VM_PKT_DATA_INBAND,
				 0);
	}

	put_pcichild(hpdev, hv_pcidev_ref_by_slot);

msi_teardown:
	list_for_each_entry(entry, &pdev->msi_list, list) {
		int i, nvec;
		if (entry->irq == 0)
			continue;
		nvec = 1 << entry->msi_attrib.multiple;
		for (i = 0; i < nvec; i++)
			destroy_irq(entry->irq + i);
	}
}

/**
 * get_bar_size() - Get the address space consumed by a BAR
 * @bar_val:	Value that a BAR returned after -1 was written
 *              to it.
 *
 * This function returns the size of the BAR, rounded up to 1
 * page.  It has to be rounded up because the hypervisor's page
 * table entry that maps the BAR into the VM can't specify an
 * offset within a page.  The invariant is that the hypervisor
 * must place any BARs of smaller than page length at the
 * beginning of a page.
 *
 * Return:	Size in bytes of the consumed MMIO space.
 */
static u64 get_bar_size(u64 bar_val)
{
	return round_up((1 + ~(bar_val & PCI_BASE_ADDRESS_MEM_MASK)),
			PAGE_SIZE);
}

/**
 * survey_child_resources() - Total all MMIO requirements
 * @hbus:	Root PCI bus, as understood by this driver
 */
static void survey_child_resources(struct hv_pcibus_device *hbus)
{
	struct list_head *iter;
	struct hv_pci_dev *hpdev;
	resource_size_t bar_size = 0;
	unsigned long flags;
	struct completion *event;
	u64 bar_val;
	int i;

	/* If nobody is waiting on the answer, don't compute it. */
	event = xchg(&hbus->survey_event, NULL);
	if (!event)
		return;

	/* If the answer has already been computed, go with it. */
	if (hbus->low_mmio_space || hbus->high_mmio_space) {
		complete(event);
		return;
	}

	spin_lock_irqsave(&hbus->device_list_lock, flags);

	/*
	 * Due to an interesting quirk of the PCI spec, all memory regions
	 * for a child device are a power of 2 in size and aligned in memory,
	 * so it's sufficient to just add them up without tracking alignment.
	 */
	list_for_each(iter, &hbus->children) {
		hpdev = container_of(iter, struct hv_pci_dev, list_entry);
		for (i = 0; i < 6; i++) {
			if (hpdev->probed_bar[i] & PCI_BASE_ADDRESS_SPACE_IO)
				dev_err(&hbus->hdev->device,
					"There's an I/O BAR in this list!\n");

			if (hpdev->probed_bar[i] != 0) {
				/*
				 * A probed BAR has all the upper bits set that
				 * can be changed.
				 */

				bar_val = hpdev->probed_bar[i];
				if (bar_val & PCI_BASE_ADDRESS_MEM_TYPE_64)
					bar_val |=
					((u64)hpdev->probed_bar[++i] << 32);
				else
					bar_val |= 0xffffffff00000000ULL;

				bar_size = get_bar_size(bar_val);

				if (bar_val & PCI_BASE_ADDRESS_MEM_TYPE_64)
					hbus->high_mmio_space += bar_size;
				else
					hbus->low_mmio_space += bar_size;
			}
		}
	}

	spin_unlock_irqrestore(&hbus->device_list_lock, flags);
	complete(event);
}

/**
 * prepopulate_bars() - Fill in BARs with defaults
 * @hbus:	Root PCI bus, as understood by this driver
 *
 * The core PCI driver code seems much, much happier if the BARs
 * for a device have values upon first scan. So fill them in.
 * The algorithm below works down from large sizes to small,
 * attempting to pack the assignments optimally. The assumption,
 * enforced in other parts of the code, is that the beginning of
 * the memory-mapped I/O space will be aligned on the largest
 * BAR size.
 */
static void prepopulate_bars(struct hv_pcibus_device *hbus)
{
	resource_size_t high_size = 0;
	resource_size_t low_size = 0;
	resource_size_t high_base = 0;
	resource_size_t low_base = 0;
	resource_size_t bar_size;
	struct hv_pci_dev *hpdev;
	struct list_head *iter;
	unsigned long flags;
	u64 bar_val;
	u32 command;
	bool high;
	int i;

	if (hbus->low_mmio_space) {
		low_size = 1ULL << (63 - __builtin_clzll(hbus->low_mmio_space));
		low_base = hbus->low_mmio_res->start;
	}

	if (hbus->high_mmio_space) {
		high_size = 1ULL <<
			(63 - __builtin_clzll(hbus->high_mmio_space));
		high_base = hbus->high_mmio_res->start;
	}

	spin_lock_irqsave(&hbus->device_list_lock, flags);

	/* Pick addresses for the BARs. */
	do {
		list_for_each(iter, &hbus->children) {
			hpdev = container_of(iter, struct hv_pci_dev,
					     list_entry);
			for (i = 0; i < 6; i++) {
				bar_val = hpdev->probed_bar[i];
				if (bar_val == 0)
					continue;
				high = bar_val & PCI_BASE_ADDRESS_MEM_TYPE_64;
				if (high) {
					bar_val |=
						((u64)hpdev->probed_bar[i + 1]
						 << 32);
				} else {
					bar_val |= 0xffffffffULL << 32;
				}
				bar_size = get_bar_size(bar_val);
				if (high) {
					if (high_size != bar_size) {
						i++;
						continue;
					}
					_hv_pcifront_write_config(hpdev,
						PCI_BASE_ADDRESS_0 + (4 * i),
						4,
						(u32)(high_base & 0xffffff00));
					i++;
					_hv_pcifront_write_config(hpdev,
						PCI_BASE_ADDRESS_0 + (4 * i),
						4, (u32)(high_base >> 32));
					high_base += bar_size;
				} else {
					if (low_size != bar_size)
						continue;
					_hv_pcifront_write_config(hpdev,
						PCI_BASE_ADDRESS_0 + (4 * i),
						4,
						(u32)(low_base & 0xffffff00));
					low_base += bar_size;
				}
			}
			if (high_size <= 1 && low_size <= 1) {
				/* Set the memory enable bit. */
				_hv_pcifront_read_config(hpdev, PCI_COMMAND, 2,
							 &command);
				command |= PCI_COMMAND_MEMORY;
				_hv_pcifront_write_config(hpdev, PCI_COMMAND, 2,
							  command);
				break;
			}
		}

		high_size >>= 1;
		low_size >>= 1;
	}  while (high_size || low_size);

	spin_unlock_irqrestore(&hbus->device_list_lock, flags);
}

/**
 * create_root_hv_pci_bus() - Expose a new root PCI bus
 * @hbus:	Root PCI bus, as understood by this driver
 *
 * Return: 0 on success, -errno on failure
 */
static int create_root_hv_pci_bus(struct hv_pcibus_device *hbus)
{
	/* Register the device */
	hbus->pci_bus = pci_create_root_bus(&hbus->hdev->device,
					    0, /* bus number is always zero */
					    &hv_pcifront_ops,
					    &hbus->sysdata,
					    &hbus->resources_for_children);
	if (!hbus->pci_bus)
		return -ENODEV;

	pci_lock_rescan_remove();
	pci_scan_child_bus(hbus->pci_bus);
	pci_bus_assign_resources(hbus->pci_bus);
	pci_bus_add_devices(hbus->pci_bus);
	pci_unlock_rescan_remove();
	hbus->state = hv_pcibus_installed;
	return 0;
}

struct q_res_req_compl {
	struct completion host_event;
	struct hv_pci_dev *hpdev;
};

/**
 * q_resource_requirements() - Query Resource Requirements
 * @context:		The completion context.
 * @resp:		The response that came from the host.
 * @resp_packet_size:	The size in bytes of resp.
 *
 * This function is invoked on completion of a Query Resource
 * Requirements packet.
 */
static void q_resource_requirements(void *context, struct pci_response *resp,
				    int resp_packet_size)
{
	struct q_res_req_compl *completion = context;
	struct pci_q_res_req_response *q_res_req =
		(struct pci_q_res_req_response *)resp;
	int i;

	if (resp->status < 0) {
		dev_err(&completion->hpdev->hbus->hdev->device,
			"query resource requirements failed: %x\n",
			resp->status);
	} else {
		for (i = 0; i < 6; i++) {
			completion->hpdev->probed_bar[i] =
				q_res_req->probed_bar[i];
		}
	}

	complete(&completion->host_event);
}

static void get_pcichild(struct hv_pci_dev *hpdev,
			    enum hv_pcidev_ref_reason reason)
{
	atomic_inc(&hpdev->refs);
}

static void put_pcichild(struct hv_pci_dev *hpdev,
			    enum hv_pcidev_ref_reason reason)
{
	if (atomic_dec_and_test(&hpdev->refs))
		kfree(hpdev);
}

/**
 * new_pcichild_device() - Create a new child device
 * @hbus:	The internal struct tracking this root PCI bus.
 * @desc:	The information supplied so far from the host
 *              about the device.
 *
 * This function creates the tracking structure for a new child
 * device and kicks off the process of figuring out what it is.
 *
 * Return: Pointer to the new tracking struct
 */
static struct hv_pci_dev *new_pcichild_device(struct hv_pcibus_device *hbus,
		struct pci_function_description *desc)
{
	struct hv_pci_dev *hpdev;
	struct pci_child_message *res_req;
	struct q_res_req_compl comp_pkt;
	struct {
		struct pci_packet init_packet;
		u8 buffer[sizeof(struct pci_child_message)];
	} pkt;
	unsigned long flags;
	int ret;

	hpdev = kzalloc(sizeof(*hpdev), GFP_ATOMIC);
	if (!hpdev)
		return NULL;

	hpdev->hbus = hbus;

	memset(&pkt, 0, sizeof(pkt));
	init_completion(&comp_pkt.host_event);
	comp_pkt.hpdev = hpdev;
	pkt.init_packet.compl_ctxt = &comp_pkt;
	pkt.init_packet.completion_func = q_resource_requirements;
	res_req = (struct pci_child_message *)&pkt.init_packet.message;
	res_req->message_type.type = PCI_QUERY_RESOURCE_REQUIREMENTS;
	res_req->wslot.slot = desc->win_slot.slot;

	ret = vmbus_sendpacket(hbus->hdev->channel, res_req,
			       sizeof(struct pci_child_message),
			       (unsigned long)&pkt.init_packet,
			       VM_PKT_DATA_INBAND,
			       VMBUS_DATA_PACKET_FLAG_COMPLETION_REQUESTED);
	if (ret)
		goto error;

	wait_for_completion(&comp_pkt.host_event);

	hpdev->desc = *desc;
	get_pcichild(hpdev, hv_pcidev_ref_initial);
	get_pcichild(hpdev, hv_pcidev_ref_childlist);
	spin_lock_irqsave(&hbus->device_list_lock, flags);
	list_add_tail(&hpdev->list_entry, &hbus->children);
	spin_unlock_irqrestore(&hbus->device_list_lock, flags);
	return hpdev;

error:
	kfree(hpdev);
	return NULL;
}

/**
 * get_pcichild_wslot() - Find device from slot
 * @hbus:	Root PCI bus, as understood by this driver
 * @wslot:	Location on the bus
 *
 * This function looks up a PCI device and returns the internal
 * representation of it.  It acquires a reference on it, so that
 * the device won't be deleted while somebody is using it.  The
 * caller is responsible for calling put_pcichild() to release
 * this reference.
 *
 * Return:	Internal representation of a PCI device
 */
static struct hv_pci_dev *get_pcichild_wslot(struct hv_pcibus_device *hbus,
					     u32 wslot)
{
	unsigned long flags;
	struct hv_pci_dev *iter, *hpdev = NULL;

	spin_lock_irqsave(&hbus->device_list_lock, flags);
	list_for_each_entry(iter, &hbus->children, list_entry) {
		if (iter->desc.win_slot.slot == wslot) {
			hpdev = iter;
			get_pcichild(hpdev, hv_pcidev_ref_by_slot);
			break;
		}
	}
	spin_unlock_irqrestore(&hbus->device_list_lock, flags);

	return hpdev;
}

/**
 * pci_devices_present_work() - Handle new list of child devices
 * @work:	Work struct embedded in struct hv_dr_work
 *
 * "Bus Relations" is the Windows term for "children of this
 * bus."  The terminology is preserved here for people trying to
 * debug the interaction between Hyper-V and Linux.  This
 * function is called when the parent partition reports a list
 * of functions that should be observed under this PCI Express
 * port (bus).
 *
 * This function updates the list, and must tolerate being
 * called multiple times with the same information.  The typical
 * number of child devices is one, with very atypical cases
 * involving three or four, so the algorithms used here can be
 * simple and inefficient.
 *
 * It must also treat the omission of a previously observed device as
 * notification that the device no longer exists.
 *
 * Note that this function is a work item, and it may not be
 * invoked in the order that it was queued.  Back to back
 * updates of the list of present devices may involve queuing
 * multiple work items, and this one may run before ones that
 * were sent later. As such, this function only does something
 * if is the last one in the queue.
 */
static void pci_devices_present_work(struct work_struct *work)
{
	u32 child_no;
	bool found;
	struct list_head *iter;
	struct pci_function_description *new_desc;
	struct hv_pci_dev *hpdev;
	struct hv_pcibus_device *hbus;
	struct list_head removed;
	struct hv_dr_work *dr_wrk;
	struct hv_dr_state *dr = NULL;
	unsigned long flags;

	dr_wrk = container_of(work, struct hv_dr_work, wrk);
	hbus = dr_wrk->bus;
	kfree(dr_wrk);

	INIT_LIST_HEAD(&removed);

	if (down_interruptible(&hbus->enum_sem)) {
		put_hvpcibus(hbus);
		return;
	}

	/* Pull this off the queue and process it if it was the last one. */
	spin_lock_irqsave(&hbus->device_list_lock, flags);
	while (!list_empty(&hbus->dr_list)) {
		dr = list_first_entry(&hbus->dr_list, struct hv_dr_state,
				      list_entry);
		list_del(&dr->list_entry);

		/* Throw this away if the list still has stuff in it. */
		if (!list_empty(&hbus->dr_list)) {
			kfree(dr);
			continue;
		}
	}
	spin_unlock_irqrestore(&hbus->device_list_lock, flags);

	if (!dr) {
		up(&hbus->enum_sem);
		put_hvpcibus(hbus);
		return;
	}

	/* First, mark all existing children as reported missing. */
	spin_lock_irqsave(&hbus->device_list_lock, flags);
	list_for_each(iter, &hbus->children) {
			hpdev = container_of(iter, struct hv_pci_dev,
					     list_entry);
			hpdev->reported_missing = true;
	}
	spin_unlock_irqrestore(&hbus->device_list_lock, flags);

	/* Next, add back any reported devices. */
	for (child_no = 0; child_no < dr->device_count; child_no++) {
		found = false;
		new_desc = &dr->func[child_no];

		spin_lock_irqsave(&hbus->device_list_lock, flags);
		list_for_each(iter, &hbus->children) {
			hpdev = container_of(iter, struct hv_pci_dev,
					     list_entry);
			if ((hpdev->desc.win_slot.slot ==
			     new_desc->win_slot.slot) &&
			    (hpdev->desc.v_id == new_desc->v_id) &&
			    (hpdev->desc.d_id == new_desc->d_id) &&
			    (hpdev->desc.ser == new_desc->ser)) {
				hpdev->reported_missing = false;
				found = true;
			}
		}
		spin_unlock_irqrestore(&hbus->device_list_lock, flags);

		if (!found) {
			hpdev = new_pcichild_device(hbus, new_desc);
			if (!hpdev)
				dev_err(&hbus->hdev->device,
					"couldn't record a child device.\n");
		}
	}

	/* Move missing children to a list on the stack. */
	spin_lock_irqsave(&hbus->device_list_lock, flags);
	do {
		found = false;
		list_for_each(iter, &hbus->children) {
			hpdev = container_of(iter, struct hv_pci_dev,
					     list_entry);
			if (hpdev->reported_missing) {
				found = true;
				put_pcichild(hpdev, hv_pcidev_ref_childlist);
				list_del(&hpdev->list_entry);
				list_add_tail(&hpdev->list_entry, &removed);
				break;
			}
		}
	} while (found);
	spin_unlock_irqrestore(&hbus->device_list_lock, flags);

	/* Delete everything that should no longer exist. */
	while (!list_empty(&removed)) {
		hpdev = list_first_entry(&removed, struct hv_pci_dev,
					 list_entry);
		list_del(&hpdev->list_entry);
		put_pcichild(hpdev, hv_pcidev_ref_initial);
	}

	switch (hbus->state) {
	case hv_pcibus_installed:
		/*
		 * Tell the core to rescan bus
		 * because there may have been changes.
		 */
		pci_lock_rescan_remove();
		pci_scan_child_bus(hbus->pci_bus);
		pci_unlock_rescan_remove();
		break;
	case hv_pcibus_init:
	case hv_pcibus_probed:
		survey_child_resources(hbus);
		break;

	default:
		break;
	}

	up(&hbus->enum_sem);
	put_hvpcibus(hbus);
	kfree(dr);
}

/**
 * hv_pci_devices_present() - Handles list of new children
 * @hbus:	Root PCI bus, as understood by this driver
 * @relations:	Packet from host listing children
 *
 * This function is invoked whenever a new list of devices for
 * this bus appears.
 */
static void hv_pci_devices_present(struct hv_pcibus_device *hbus,
				   struct pci_bus_relations *relations)
{
	struct hv_dr_state *dr;
	struct hv_dr_work *dr_wrk;
	unsigned long flags;

	dr_wrk = kzalloc(sizeof(*dr_wrk), GFP_NOWAIT);
	if (!dr_wrk)
		return;

	dr = kzalloc(offsetof(struct hv_dr_state, func) +
		     (sizeof(struct pci_function_description) *
		      (relations->device_count)), GFP_NOWAIT);
	if (!dr)  {
		kfree(dr_wrk);
		return;
	}

	INIT_WORK(&dr_wrk->wrk, pci_devices_present_work);
	dr_wrk->bus = hbus;
	dr->device_count = relations->device_count;
	if (dr->device_count != 0) {
		memcpy(dr->func, relations->func,
		       sizeof(struct pci_function_description) *
		       dr->device_count);
	}

	spin_lock_irqsave(&hbus->device_list_lock, flags);
	list_add_tail(&dr->list_entry, &hbus->dr_list);
	spin_unlock_irqrestore(&hbus->device_list_lock, flags);

	get_hvpcibus(hbus);
	schedule_work(&dr_wrk->wrk);
}

/**
 * hv_eject_device_work() - Asynchronously handles ejection
 * @work:	Work struct embedded in internal device struct
 *
 * This function handles ejecting a device.  Windows will
 * attempt to gracefully eject a device, waiting 60 seconds to
 * hear back from the guest OS that this completed successfully.
 * If this timer expires, the device will be forcibly removed.
 */
static void hv_eject_device_work(struct work_struct *work)
{
	struct pci_eject_response *ejct_pkt;
	struct hv_pci_dev *hpdev;
	struct pci_dev *pdev;
	unsigned long flags;
	int wslot;
	struct {
		struct pci_packet pkt;
		u8 buffer[sizeof(struct pci_eject_response)];
	} ctxt;

	hpdev = container_of(work, struct hv_pci_dev, wrk);

	if (hpdev->state != hv_pcichild_ejecting) {
		put_pcichild(hpdev, hv_pcidev_ref_pnp);
		return;
	}

	/*
	 * Ejection can come before or after the PCI bus has been set up, so
	 * attempt to find it and tear down the bus state, if it exists.  This
	 * must be done without constructs like pci_domain_nr(hbus->pci_bus)
	 * because hbus->pci_bus may not exist yet.
	 */
	wslot = wslot_to_devfn(hpdev->desc.win_slot.slot);
	pdev = pci_get_domain_bus_and_slot(hpdev->hbus->sysdata.domain, 0,
					   wslot);
	if (pdev) {
		pci_lock_rescan_remove();
		pci_stop_and_remove_bus_device(pdev);
		pci_dev_put(pdev);
		pci_unlock_rescan_remove();
	}

	memset(&ctxt, 0, sizeof(ctxt));
	ejct_pkt = (struct pci_eject_response *)&ctxt.pkt.message;
	ejct_pkt->message_type.type = PCI_EJECTION_COMPLETE;
	ejct_pkt->wslot.slot = hpdev->desc.win_slot.slot;
	vmbus_sendpacket(hpdev->hbus->hdev->channel, ejct_pkt,
			 sizeof(*ejct_pkt), (unsigned long)&ctxt.pkt,
			 VM_PKT_DATA_INBAND, 0);

	spin_lock_irqsave(&hpdev->hbus->device_list_lock, flags);
	list_del(&hpdev->list_entry);
	spin_unlock_irqrestore(&hpdev->hbus->device_list_lock, flags);

	put_pcichild(hpdev, hv_pcidev_ref_childlist);
	put_pcichild(hpdev, hv_pcidev_ref_pnp);
	put_hvpcibus(hpdev->hbus);
}

/**
 * hv_pci_eject_device() - Handles device ejection
 * @hpdev:	Internal device tracking struct
 *
 * This function is invoked when an ejection packet arrives.  It
 * just schedules work so that we don't re-enter the packet
 * delivery code handling the ejection.
 */
static void hv_pci_eject_device(struct hv_pci_dev *hpdev)
{
	hpdev->state = hv_pcichild_ejecting;
	get_pcichild(hpdev, hv_pcidev_ref_pnp);
	INIT_WORK(&hpdev->wrk, hv_eject_device_work);
	get_hvpcibus(hpdev->hbus);
	schedule_work(&hpdev->wrk);
}

/**
 * hv_pci_onchannelcallback() - Handles incoming packets
 * @context:	Internal bus tracking struct
 *
 * This function is invoked whenever the host sends a packet to
 * this channel (which is private to this root PCI bus).
 */
static void hv_pci_onchannelcallback(void *context)
{
	const int packet_size = 0x100;
	int ret;
	struct hv_pcibus_device *hbus = context;
	u32 bytes_recvd;
	u64 req_id;
	struct vmpacket_descriptor *desc;
	unsigned char *buffer;
	int bufferlen = packet_size;
	struct pci_packet *comp_packet;
	struct pci_response *response;
	struct pci_incoming_message *new_message;
	struct pci_bus_relations *bus_rel;
	struct pci_dev_incoming *dev_message;
	struct hv_pci_dev *hpdev;

	buffer = kmalloc(bufferlen, GFP_ATOMIC);
	if (!buffer)
		return;

	while (1) {
		ret = vmbus_recvpacket_raw(hbus->hdev->channel, buffer,
					   bufferlen, &bytes_recvd, &req_id);

		if (ret == -ENOBUFS) {
			kfree(buffer);
			/* Handle large packet */
			bufferlen = bytes_recvd;
			buffer = kmalloc(bytes_recvd, GFP_ATOMIC);
			if (!buffer)
				return;
			continue;
		}

		/* Zero length indicates there are no more packets. */
		if (ret || !bytes_recvd)
			break;

		/*
		 * All incoming packets must be at least as large as a
		 * response.
		 */
		if (bytes_recvd <= sizeof(struct pci_response))
			continue;
		desc = (struct vmpacket_descriptor *)buffer;

		switch (desc->type) {
		case VM_PKT_COMP:

			/*
			 * The host is trusted, and thus it's safe to interpret
			 * this transaction ID as a pointer.
			 */
			comp_packet = (struct pci_packet *)req_id;
			response = (struct pci_response *)buffer;
			comp_packet->completion_func(comp_packet->compl_ctxt,
						     response,
						     bytes_recvd);
			break;

		case VM_PKT_DATA_INBAND:

			new_message = (struct pci_incoming_message *)buffer;
			switch (new_message->message_type.type) {
			case PCI_BUS_RELATIONS:

				bus_rel = (struct pci_bus_relations *)buffer;
				if (bytes_recvd <
				    offsetof(struct pci_bus_relations, func) +
				    (sizeof(struct pci_function_description) *
				     (bus_rel->device_count))) {
					dev_err(&hbus->hdev->device,
						"bus relations too small\n");
					break;
				}

				hv_pci_devices_present(hbus, bus_rel);
				break;

			case PCI_EJECT:

				dev_message = (struct pci_dev_incoming *)buffer;
				hpdev = get_pcichild_wslot(hbus,
						      dev_message->wslot.slot);
				if (hpdev) {
					hv_pci_eject_device(hpdev);
					put_pcichild(hpdev,
							hv_pcidev_ref_by_slot);
				}
				break;

			default:
				dev_warn(&hbus->hdev->device,
					"Unimplemented protocol message %x\n",
					new_message->message_type.type);
				break;
			}
			break;

		default:
			dev_err(&hbus->hdev->device,
				"unhandled packet type %d, tid %llx len %d\n",
				desc->type, req_id, bytes_recvd);
			break;
		}
	}

	kfree(buffer);
}

/**
 * hv_pci_protocol_negotiation() - Set up protocol
 * @hdev:	VMBus's tracking struct for this root PCI bus
 *
 * This driver is intended to support running on Windows 10
 * (server) and later versions. It will not run on earlier
 * versions, as they assume that many of the operations which
 * Linux needs accomplished with a spinlock held were done via
 * asynchronous messaging via VMBus.  Windows 10 increases the
 * surface area of PCI emulation so that these actions can take
 * place by suspending a virtual processor for their duration.
 *
 * This function negotiates the channel protocol version,
 * failing if the host doesn't support the necessary protocol
 * level.
 */
static int hv_pci_protocol_negotiation(struct hv_device *hdev)
{
	size_t i;
	struct pci_version_request *version_req;
	struct hv_pci_compl comp_pkt;
	struct pci_packet *pkt;
	int ret;

	/*
	 * Initiate the handshake with the host and negotiate
	 * a version that the host can support. We start with the
	 * highest version number and go down if the host cannot
	 * support it.
	 */
	pkt = kzalloc(sizeof(*pkt) + sizeof(*version_req), GFP_KERNEL);
	if (!pkt)
		return -ENOMEM;

	init_completion(&comp_pkt.host_event);
	pkt->completion_func = hv_pci_generic_compl;
	pkt->compl_ctxt = &comp_pkt;
	version_req = (struct pci_version_request *)&pkt->message;
	version_req->message_type.type = PCI_QUERY_PROTOCOL_VERSION;
	for (i = 0; i < ARRAY_SIZE(pci_protocol_versions); i++) {
		dev_info(&hdev->device, "PCI VMBus probing version %x\n",
			pci_protocol_versions[i]);
		version_req->protocol_version = pci_protocol_versions[i];
		ret = vmbus_sendpacket(
			hdev->channel, version_req,
			sizeof(struct pci_version_request),
			(unsigned long)pkt, VM_PKT_DATA_INBAND,
			VMBUS_DATA_PACKET_FLAG_COMPLETION_REQUESTED);
		if (ret)
			goto exit;

		wait_for_completion(&comp_pkt.host_event);

		dev_info(&hdev->device,
			"PCI VMBus probing result version %x: %#x\n",
			pci_protocol_versions[i], comp_pkt.completion_status);

		if (comp_pkt.completion_status >= 0) {
			pci_protocol_version = pci_protocol_versions[i];
			break;
		}

		if (comp_pkt.completion_status != STATUS_REVISION_MISMATCH) {
			dev_err(&hdev->device,
				"PCI Pass-through VSP failed version request: %#x\n",
				comp_pkt.completion_status);
			ret = -EPROTO;
			break;
		}

		reinit_completion(&comp_pkt.host_event);
	}

exit:
	dev_info(&hdev->device,
		"PCI VMBus probing: Using version %#x\n",
		pci_protocol_version);

	kfree(pkt);
	return ret;
}

/**
 * hv_pci_free_bridge_windows() - Release memory regions for the
 * bus
 * @hbus:	Root PCI bus, as understood by this driver
 */
static void hv_pci_free_bridge_windows(struct hv_pcibus_device *hbus)
{
	/*
	 * Set the resources back to the way they looked when they
	 * were allocated by setting IORESOURCE_BUSY again.
	 */

	if (hbus->low_mmio_space && hbus->low_mmio_res) {
		hbus->low_mmio_res->flags |= IORESOURCE_BUSY;
		release_mem_region(hbus->low_mmio_res->start,
				   resource_size(hbus->low_mmio_res));
	}

	if (hbus->high_mmio_space && hbus->high_mmio_res) {
		hbus->high_mmio_res->flags |= IORESOURCE_BUSY;
		release_mem_region(hbus->high_mmio_res->start,
				   resource_size(hbus->high_mmio_res));
	}
}

/**
 * hv_pci_allocate_bridge_windows() - Allocate memory regions
 * for the bus
 * @hbus:	Root PCI bus, as understood by this driver
 *
 * This function calls vmbus_allocate_mmio(), which is itself a
 * bit of a compromise.  Ideally, we might change the pnp layer
 * in the kernel such that it comprehends either PCI devices
 * which are "grandchildren of ACPI," with some intermediate bus
 * node (in this case, VMBus) or change it such that it
 * understands VMBus.  The pnp layer, however, has been declared
 * deprecated, and not subject to change.
 *
 * The workaround, implemented here, is to ask VMBus to allocate
 * MMIO space for this bus.  VMBus itself knows which ranges are
 * appropriate by looking at its own ACPI objects.  Then, after
 * these ranges are claimed, they're modified to look like they
 * would have looked if the ACPI and pnp code had allocated
 * bridge windows.  These descriptors have to exist in this form
 * in order to satisfy the code which will get invoked when the
 * endpoint PCI function driver calls request_mem_region() or
 * request_mem_region_exclusive().
 *
 * Return: 0 on success, -errno on failure
 */
static int hv_pci_allocate_bridge_windows(struct hv_pcibus_device *hbus)
{
	resource_size_t align;
	int ret;

	if (hbus->low_mmio_space) {
		align = 1ULL << (63 - __builtin_clzll(hbus->low_mmio_space));
		ret = vmbus_allocate_mmio(&hbus->low_mmio_res, hbus->hdev, 0,
					  (u64)(u32)0xffffffff,
					  hbus->low_mmio_space,
					  align, false);
		if (ret) {
			dev_err(&hbus->hdev->device,
				"Need %#llx of low MMIO space. Consider reconfiguring the VM.\n",
				hbus->low_mmio_space);
			return ret;
		}

		/* Modify this resource to become a bridge window. */
		hbus->low_mmio_res->flags |= (IORESOURCE_MEM | IORESOURCE_WINDOW);
		hbus->low_mmio_res->flags &= ~IORESOURCE_BUSY;
		pci_add_resource(&hbus->resources_for_children,
				 hbus->low_mmio_res);
	}

	if (hbus->high_mmio_space) {
		align = 1ULL << (63 - __builtin_clzll(hbus->high_mmio_space));
		ret = vmbus_allocate_mmio(&hbus->high_mmio_res, hbus->hdev,
					  0x100000000, -1,
					  hbus->high_mmio_space, align,
					  false);
		if (ret) {
			dev_err(&hbus->hdev->device,
				"Need %#llx of high MMIO space. Consider reconfiguring the VM.\n",
				hbus->high_mmio_space);
			goto release_low_mmio;
		}

		/* Modify this resource to become a bridge window. */
		hbus->high_mmio_res->flags |= (IORESOURCE_MEM | IORESOURCE_WINDOW);
		hbus->high_mmio_res->flags &= ~IORESOURCE_BUSY;
		pci_add_resource(&hbus->resources_for_children,
				 hbus->high_mmio_res);
	}

	return 0;

release_low_mmio:
	if (hbus->low_mmio_res) {
		release_mem_region(hbus->low_mmio_res->start,
				   resource_size(hbus->low_mmio_res));
	}

	return ret;
}

/**
 * hv_allocate_config_window() - Find MMIO space for PCI Config
 * @hbus:	Root PCI bus, as understood by this driver
 *
 * This function claims memory-mapped I/O space for accessing
 * configuration space for the functions on this bus.
 *
 * Return: 0 on success, -errno on failure
 */
static int hv_allocate_config_window(struct hv_pcibus_device *hbus)
{
	int ret;

	/*
	 * Set up a region of MMIO space to use for accessing configuration
	 * space.
	 */
	ret = vmbus_allocate_mmio(&hbus->mem_config, hbus->hdev, 0, -1,
				  PCI_CONFIG_MMIO_LENGTH, 0x1000, false);
	if (ret)
		return ret;

	/*
	 * vmbus_allocate_mmio() gets used for allocating both device endpoint
	 * resource claims (those which cannot be overlapped) and the ranges
	 * which are valid for the children of this bus, which are intended
	 * to be overlapped by those children.  Set the flag on this claim
	 * meaning that this region can't be overlapped.
	 */

	hbus->mem_config->flags |= IORESOURCE_BUSY;

	return 0;
}

static void hv_free_config_window(struct hv_pcibus_device *hbus)
{
	release_mem_region(hbus->mem_config->start, PCI_CONFIG_MMIO_LENGTH);
}

/**
 * hv_pci_enter_d0() - Bring the "bus" into the D0 power state
 * @hdev:	VMBus's tracking struct for this root PCI bus
 *
 * Return: 0 on success, -errno on failure
 */
static int hv_pci_enter_d0(struct hv_device *hdev)
{
	struct hv_pcibus_device *hbus = hv_get_drvdata(hdev);
	struct pci_bus_d0_entry *d0_entry;
	struct hv_pci_compl comp_pkt;
	struct pci_packet *pkt;
	int ret;

	/*
	 * Tell the host that the bus is ready to use, and moved into the
	 * powered-on state.  This includes telling the host which region
	 * of memory-mapped I/O space has been chosen for configuration space
	 * access.
	 */
	pkt = kzalloc(sizeof(*pkt) + sizeof(*d0_entry), GFP_KERNEL);
	if (!pkt)
		return -ENOMEM;

	init_completion(&comp_pkt.host_event);
	pkt->completion_func = hv_pci_generic_compl;
	pkt->compl_ctxt = &comp_pkt;
	d0_entry = (struct pci_bus_d0_entry *)&pkt->message;
	d0_entry->message_type.type = PCI_BUS_D0ENTRY;
	d0_entry->mmio_base = hbus->mem_config->start;

	ret = vmbus_sendpacket(hdev->channel, d0_entry, sizeof(*d0_entry),
			       (unsigned long)pkt, VM_PKT_DATA_INBAND,
			       VMBUS_DATA_PACKET_FLAG_COMPLETION_REQUESTED);
	if (ret)
		goto exit;

	wait_for_completion(&comp_pkt.host_event);

	if (comp_pkt.completion_status < 0) {
		dev_err(&hdev->device,
			"PCI Pass-through VSP failed D0 Entry with status %x\n",
			comp_pkt.completion_status);
		ret = -EPROTO;
		goto exit;
	}

	ret = 0;

exit:
	kfree(pkt);
	return ret;
}

/**
 * hv_pci_query_relations() - Ask host to send list of child
 * devices
 * @hdev:	VMBus's tracking struct for this root PCI bus
 *
 * Return: 0 on success, -errno on failure
 */
static int hv_pci_query_relations(struct hv_device *hdev)
{
	struct hv_pcibus_device *hbus = hv_get_drvdata(hdev);
	struct pci_message message;
	struct completion comp;
	int ret;

	/* Ask the host to send along the list of child devices */
	init_completion(&comp);
	if (cmpxchg(&hbus->survey_event, NULL, &comp))
		return -ENOTEMPTY;

	memset(&message, 0, sizeof(message));
	message.type = PCI_QUERY_BUS_RELATIONS;

	ret = vmbus_sendpacket(hdev->channel, &message, sizeof(message),
			       0, VM_PKT_DATA_INBAND, 0);
	if (ret)
		return ret;

	wait_for_completion(&comp);
	return 0;
}

/**
 * hv_send_resources_allocated() - Report local resource choices
 * @hdev:	VMBus's tracking struct for this root PCI bus
 *
 * The host OS is expecting to be sent a request as a message
 * which contains all the resources that the device will use.
 * The response contains those same resources, "translated"
 * which is to say, the values which should be used by the
 * hardware, when it delivers an interrupt.  (MMIO resources are
 * used in local terms.)  This is nice for Windows, and lines up
 * with the FDO/PDO split, which doesn't exist in Linux.  Linux
 * is deeply expecting to scan an emulated PCI configuration
 * space.  So this message is sent here only to drive the state
 * machine on the host forward.
 *
 * Return: 0 on success, -errno on failure
 */
static int hv_send_resources_allocated(struct hv_device *hdev)
{
	struct hv_pcibus_device *hbus = hv_get_drvdata(hdev);
	struct pci_resources_assigned *res_assigned;
	struct pci_resources_assigned2 *res_assigned2;
	struct hv_pci_compl comp_pkt;
	struct hv_pci_dev *hpdev;
	struct pci_packet *pkt;
	u32 wslot;
	int ret;
	size_t sizeRes;

	sizeRes = (pci_protocol_version < PCI_PROTOCOL_VERSION_1_2)
			? sizeof(*res_assigned) : sizeof(*res_assigned2);

	pkt = kmalloc(sizeof(*pkt) + sizeRes, GFP_KERNEL);
	if (!pkt)
		return -ENOMEM;

	ret = 0;

	for (wslot = 0; wslot < 256; wslot++) {
		hpdev = get_pcichild_wslot(hbus, wslot);
		if (!hpdev)
			continue;

		memset(pkt, 0, sizeof(*pkt) + sizeRes);
		init_completion(&comp_pkt.host_event);
		pkt->completion_func = hv_pci_generic_compl;
		pkt->compl_ctxt = &comp_pkt;
		if (pci_protocol_version < PCI_PROTOCOL_VERSION_1_2) {
			res_assigned =
				(struct pci_resources_assigned *)&pkt->message;
			res_assigned->message_type.type =
				PCI_RESOURCES_ASSIGNED;
			res_assigned->wslot.slot = hpdev->desc.win_slot.slot;
		} else {
			res_assigned2 =
				(struct pci_resources_assigned2 *)&pkt->message;
			res_assigned2->message_type.type =
				PCI_RESOURCES_ASSIGNED2;
			res_assigned2->wslot.slot = hpdev->desc.win_slot.slot;
		}

		put_pcichild(hpdev, hv_pcidev_ref_by_slot);

		ret = vmbus_sendpacket(
			hdev->channel, &pkt->message,
			sizeRes,
			(unsigned long)pkt,
			VM_PKT_DATA_INBAND,
			VMBUS_DATA_PACKET_FLAG_COMPLETION_REQUESTED);
		if (ret)
			break;

		wait_for_completion(&comp_pkt.host_event);

		if (comp_pkt.completion_status < 0) {
			ret = -EPROTO;
			dev_err(&hdev->device,
				"resource allocated returned 0x%x",
				comp_pkt.completion_status);
			break;
		}
	}

	kfree(pkt);
	return ret;
}

/**
 * hv_send_resources_released() - Report local resources
 * released
 * @hdev:	VMBus's tracking struct for this root PCI bus
 *
 * Return: 0 on success, -errno on failure
 */
static int hv_send_resources_released(struct hv_device *hdev)
{
	struct hv_pcibus_device *hbus = hv_get_drvdata(hdev);
	struct pci_child_message pkt;
	struct hv_pci_dev *hpdev;
	u32 wslot;
	int ret;

	for (wslot = 0; wslot < 256; wslot++) {
		hpdev = get_pcichild_wslot(hbus, wslot);
		if (!hpdev)
			continue;

		memset(&pkt, 0, sizeof(pkt));
		pkt.message_type.type = PCI_RESOURCES_RELEASED;
		pkt.wslot.slot = hpdev->desc.win_slot.slot;

		put_pcichild(hpdev, hv_pcidev_ref_by_slot);

		ret = vmbus_sendpacket(hdev->channel, &pkt, sizeof(pkt), 0,
				       VM_PKT_DATA_INBAND, 0);
		if (ret)
			return ret;
	}

	return 0;
}

static void get_hvpcibus(struct hv_pcibus_device *hbus)
{
	atomic_inc(&hbus->remove_lock);
}

static void put_hvpcibus(struct hv_pcibus_device *hbus)
{
	if (atomic_dec_and_test(&hbus->remove_lock))
		complete(&hbus->remove_event);
}

/**
 * hv_pci_probe() - New VMBus channel probe, for a root PCI bus
 * @hdev:	VMBus's tracking struct for this root PCI bus
 * @dev_id:	Identifies the device itself
 *
 * Return: 0 on success, -errno on failure
 */
static int hv_pci_probe(struct hv_device *hdev,
			const struct hv_vmbus_device_id *dev_id)
{
	struct hv_pcibus_device *hbus;
	int ret;

	/*
	 * hv_pcibus_device contains the hypercall arguments for retargeting in
	 * hv_irq_unmask(). Those must not cross a page boundary.
	 */
	BUILD_BUG_ON(sizeof(*hbus) > PAGE_SIZE);

	hbus = (struct hv_pcibus_device *)get_zeroed_page(GFP_KERNEL);
	if (!hbus)
		return -ENOMEM;
	hbus->state = hv_pcibus_init;

	/*
	 * The PCI bus "domain" is what is called "segment" in ACPI and
	 * other specs.  Pull it from the instance ID, to get something
	 * unique.  Bytes 8 and 9 are what is used in Windows guests, so
	 * do the same thing for consistency.  Note that, since this code
	 * only runs in a Hyper-V VM, Hyper-V can (and does) guarantee
	 * that (1) the only domain in use for something that looks like
	 * a physical PCI bus (which is actually emulated by the
	 * hypervisor) is domain 0 and (2) there will be no overlap
	 * between domains derived from these instance IDs in the same
	 * VM.
	 */
	hbus->sysdata.domain = hdev->dev_instance.b[9] |
			       hdev->dev_instance.b[8] << 8;

	hbus->hdev = hdev;
	atomic_inc(&hbus->remove_lock);
	INIT_LIST_HEAD(&hbus->children);
	INIT_LIST_HEAD(&hbus->dr_list);
	INIT_LIST_HEAD(&hbus->resources_for_children);
	spin_lock_init(&hbus->config_lock);
	spin_lock_init(&hbus->device_list_lock);
	spin_lock_init(&hbus->retarget_msi_interrupt_lock);
	sema_init(&hbus->enum_sem, 1);
	init_completion(&hbus->remove_event);

	ret = vmbus_open(hdev->channel, pci_ring_size, pci_ring_size, NULL, 0,
			 hv_pci_onchannelcallback, hbus);
	if (ret)
		goto free_bus;

	hv_set_drvdata(hdev, hbus);

	ret = hv_pci_protocol_negotiation(hdev);
	if (ret)
		goto close;

	ret = hv_allocate_config_window(hbus);
	if (ret)
		goto close;

	hbus->cfg_addr = ioremap(hbus->mem_config->start,
				 PCI_CONFIG_MMIO_LENGTH);
	if (!hbus->cfg_addr) {
		dev_err(&hdev->device,
			"Unable to map a virtual address for config space\n");
		ret = -ENOMEM;
		goto release;
	}

	ret = hv_pci_query_relations(hdev);
	if (ret)
		goto unmap;

	ret = hv_pci_enter_d0(hdev);
	if (ret)
		goto unmap;

	ret = hv_pci_allocate_bridge_windows(hbus);
	if (ret)
		goto unmap;

	ret = hv_send_resources_allocated(hdev);
	if (ret)
		goto free_windows;

	prepopulate_bars(hbus);

	hbus->state = hv_pcibus_probed;

	ret = create_root_hv_pci_bus(hbus);
	if (ret)
		goto free_windows;

	return 0;

free_windows:
	hv_pci_free_bridge_windows(hbus);
unmap:
	iounmap(hbus->cfg_addr);
release:
	hv_free_config_window(hbus);
close:
	vmbus_close(hdev->channel);
free_bus:
	free_page((unsigned long)hbus);
	return ret;
}

/**
 * hv_pci_remove() - Remove routine for this VMBus channel
 * @hdev:	VMBus's tracking struct for this root PCI bus
 *
 * Return: 0 on success, -errno on failure
 */
static int hv_pci_remove(struct hv_device *hdev)
{
	int ret;
	struct hv_pcibus_device *hbus;
	union {
		struct pci_packet teardown_packet;
		u8 buffer[0x100];
	} pkt;
	struct pci_bus_relations relations;
	struct hv_pci_compl comp_pkt;

	hbus = hv_get_drvdata(hdev);

	memset(&pkt.teardown_packet, 0, sizeof(pkt.teardown_packet));
	init_completion(&comp_pkt.host_event);
	pkt.teardown_packet.completion_func = hv_pci_generic_compl;
	pkt.teardown_packet.compl_ctxt = &comp_pkt;
	pkt.teardown_packet.message[0].type = PCI_BUS_D0EXIT;

	ret = vmbus_sendpacket(hdev->channel, &pkt.teardown_packet.message,
			       sizeof(struct pci_message),
			       (unsigned long)&pkt.teardown_packet,
			       VM_PKT_DATA_INBAND,
			       VMBUS_DATA_PACKET_FLAG_COMPLETION_REQUESTED);
	if (!ret)
		wait_for_completion_timeout(&comp_pkt.host_event, 10 * HZ);

	if (hbus->state == hv_pcibus_installed) {
		/* Remove the bus from PCI's point of view. */
		pci_lock_rescan_remove();
//		pci_stop_root_bus(hbus->pci_bus);
//		pci_remove_root_bus(hbus->pci_bus);
		pci_unlock_rescan_remove();
		hbus->state = hv_pcibus_removed;
	}

	ret = hv_send_resources_released(hdev);
	if (ret)
		dev_err(&hdev->device,
			"Couldn't send resources released packet(s)\n");

	vmbus_close(hdev->channel);

	/* Delete any children which might still exist. */
	memset(&relations, 0, sizeof(relations));
	hv_pci_devices_present(hbus, &relations);

	iounmap(hbus->cfg_addr);
	hv_free_config_window(hbus);
	pci_free_resource_list(&hbus->resources_for_children);
	hv_pci_free_bridge_windows(hbus);
	put_hvpcibus(hbus);
	wait_for_completion(&hbus->remove_event);
	free_page((unsigned long)hbus);
	return 0;
}

static const struct hv_vmbus_device_id hv_pci_id_table[] = {
	/* PCI Pass-through Class ID */
	/* 44C4F61D-4444-4400-9D52-802E27EDE19F */
	{ HV_PCIE_GUID, },
	{ },
};

MODULE_DEVICE_TABLE(vmbus, hv_pci_id_table);

static struct hv_driver hv_pci_drv = {
	.name		= "hv_pci",
	.id_table	= hv_pci_id_table,
	.probe		= hv_pci_probe,
	.remove		= hv_pci_remove,
};

static void __exit exit_hv_pci_drv(void)
{
	vmbus_driver_unregister(&hv_pci_drv);
}

static void *(*text_poke_ptr)(void *addr, const void *opcode, size_t len);
static void hook_func(void *old, void *new)
{
	unsigned char trampoline[6];
	memcpy(trampoline, "\x68\x00\x00\x00\x00\xc3", 6);

	//using only the low 32 bits of "new" ?
	*((unsigned int *)&trampoline[1]) = (unsigned long) new;

	text_poke_ptr(old, trampoline, 6);
}

static void *smi;
static void *cmm;
static void *tmi;
static struct irq_chip *msi_chip_ptr;
static struct irq_chip *msi_ir_chip_ptr;

static int sym_callback(void *data, const char *name, struct module *mod, unsigned long addr)
{
	if (strcmp("irq_to_desc", name) == 0) {
		irq_to_desc_ptr = (void *)addr;
		printk(KERN_ERR "itd %p\n", irq_to_desc_ptr);
	} else if (strcmp("pci_create_root_bus", name) == 0) {
		pci_create_root_bus_ptr = (void *)addr;
		printk(KERN_ERR "pcrb %p\n", pci_create_root_bus_ptr);
	} else if (strcmp("destroy_irq", name) == 0) {
		destroy_irq_ptr = (void *)addr;
		printk(KERN_ERR "destroy_irq %p\n", destroy_irq);
	} else if (strcmp("set_desc_affinity", name) == 0) {
		set_desc_affinity_ptr = (void *)addr;
		printk(KERN_ERR "aff %p\n", set_desc_affinity_ptr);
        } else if (strcmp("arch_setup_msi_irqs", name) == 0) {
                smi = (void *)addr;
                printk(KERN_ERR "smi %p\n", smi);
        } else if (strcmp("msi_compose_msg", name) == 0) {
                cmm = (void *)addr;
                printk(KERN_ERR "cmm %p\n", cmm);
        } else if (strcmp("arch_teardown_msi_irqs", name) == 0) {
                tmi = (void *)addr;
                printk(KERN_ERR "tmi %p\n", tmi);
	} else if (strcmp("text_poke_smp", name) == 0) {
		text_poke_ptr = (void *)addr;
	} else if (strcmp("msi_chip", name) == 0) {
		printk(KERN_ERR "msi_chip: %p\n", (void*) addr);
		msi_chip_ptr = (void *)addr;
	} else if (strcmp("msi_ir_chip", name) == 0) {
		printk(KERN_ERR "msi_ir_chip: %p\n", (void*) addr);
		msi_ir_chip_ptr = (void *)addr;
        }
	return 0;
}

static int __init init_hv_pci_drv(void)
{
	/*
	 * Hook the MSI management functions so that interrupts can be
	 * remapped through the hypervisor.
	 */

	int r = kallsyms_on_each_symbol(sym_callback, NULL);
	if (r)
		return r;

	msi_chip_ptr->set_affinity = hv_set_affinity;
	msi_ir_chip_ptr->set_affinity = hv_set_affinity;

	// TODO: Resolve these 3 funcs
	//hook_func(smi, hv_setup_msi_irqs);
	hook_func(cmm, hv_compose_msi_msg);
	hook_func(tmi, hv_teardown_msi_irqs);

	/* Register this driver with VMBus. */
	return vmbus_driver_register(&hv_pci_drv);
}

module_init(init_hv_pci_drv);
module_exit(exit_hv_pci_drv);

MODULE_DESCRIPTION("Hyper-V PCI");
MODULE_LICENSE("GPL v2");
