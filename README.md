# AOR2 Hypervisor Project

A minimal x86-64 hypervisor built on Linux's **KVM API**, written for the *Computer Architecture and
Organization 2* (AOR2) course at the School of Electrical Engineering, University of Belgrade.

The hypervisor (**host**) boots a small freestanding 64-bit **guest** binary directly in long mode,
without a bootloader, BIOS, or OS — the host sets up paging, segments, and registers by hand and then
runs the guest's raw machine code inside a KVM virtual machine.

The assignment is split into three graded phases — **A**, **B**, and **C** — each building on the
previous one.

## Project layout

```
project/
├── guest/              guest-side code, compiled into a flat binary image
│   ├── inc/
│   │   ├── descriptors.h   IDT/GDT/interrupt-frame structures
│   │   ├── interrupts.h
│   │   └── io.h            in/out port helpers
│   ├── src/
│   │   ├── main.c          guest entry point (_start)
│   │   └── interrupts.c    IDT setup + interrupt handlers
│   ├── guest.ld            linker script (flat binary, linked at 0x8000)
│   └── Makefile
└── host/                host-side hypervisor code
    ├── inc/
    │   ├── vm.h             VM struct, paging/segment constants
    │   └── parser.h          command-line argument parsing
    ├── src/
    │   ├── main.c            VM lifecycle, KVM_RUN loop, exit-reason handling
    │   ├── vm.c              KVM setup, long-mode paging, guest image loading
    │   └── parser.c          command-line argument parsing
    └── Makefile
```

Running `make` in each directory produces:

- `host/build/hypervisor` — the hypervisor executable
- `guest/build/guest.img` — the flat guest binary loaded into guest memory

## Building and running

```sh
cd guest && make
cd ../host && make
./host/build/hypervisor -m <2|4|8> -p <4|2> -g <guest-image> [guest-image ...]
```

Example:

```sh
./host/build/hypervisor --memory 4 --page 2 --guest guest1.img guest2.img
```

## Phase A — Core hypervisor features

- Guest physical memory of **2MB, 4MB, or 8MB**, selected via `-m`/`--memory`.
- VM runs in 64-bit **long mode**.
- Page size of **4KB or 2MB**, selected via `-p`/`--page`.
- A single virtual CPU per VM.
- Serial-style I/O on port `0xE9`, one byte at a time.
- Guest execution ends only via the `hlt` instruction.
- Loading and running one or more guest images, given via `-g`/`--guest` (one VM per image).
- One POSIX thread (`pthread`) per running VM.
- Clean shutdown and an error message on any unexpected VM exit.
- Reject invalid option values (e.g. `--memory 3`) with an error message.

## Phase B — File I/O support

Extends Phase A so the guest can open, close, read, write, and seek within files on the host,
via `IN`/`OUT` instructions on I/O port `0x0278`:

```c
int open(const char *path, int flags);
int close(int fd);
int read(int fd, char *buf, int count);
int write(int fd, const char *buf, int count);
int lseek(int fd, const int offset, int off_flag);
```

- `open` flags: `O_RD=1`, `O_WR=2`, `O_RDWR=4`, `O_CREATE=8`.
- `lseek` flags: `SEEK_SET=1`, `SEEK_END=2`.
- File position starts at 0 on open.
- Filenames: letters, digits `0`–`9`, and `.`; must start with a letter — otherwise the guest gets
  an error back.
- Local and shared files are stored on the host filesystem.
- Shared files are passed to the hypervisor via `-f`/`--file` and are shared between VMs; the first
  write from any VM triggers **copy-on-write**, after which that VM works on its own local copy.
- The hypervisor must ensure a guest can only access files it created or has read access to.

Example:

```sh
./hypervisor -m 4 -p 2 -g guest1.img guest2.img --file a.txt b.txt
```

## Phase C — Inter-VM communication via interrupts

Extends Phase B with a shared buffer (size `BUFFER_SIZE`) that VMs use to pass data to each other,
mediated by the hypervisor and delivered via interrupt vector 32:

- Each guest is assigned a role on first handling of the interrupt, reported back via port `0x510`:
  **reader** (`0`) or **writer** (`1`). Exactly one VM is the writer; all others are readers.
- **Writer**: writes a 32-bit byte count followed by that many bytes to port `0x510`, then reads
  back the number of bytes actually accepted from port `0x520`. If the write exceeds
  `BUFFER_SIZE`, the excess is dropped by the hypervisor.
- **Reader**: reads the byte count and then the bytes from port `0x510`, then reports the number of
  bytes it actually read back via port `0x520`; if it doesn't read everything, that VM is stopped.
- The writer only resumes writing once all readers have finished reading.

Intended test setup: one VM reads from a local/shared file and writes it into the shared buffer;
the remaining VMs read from the shared buffer over interrupts and write what they read into a local
file.

## Error handling

At every phase, invalid command-line option values (e.g. `--memory 3`) must produce an error message
and stop execution rather than proceeding with an undefined configuration.
