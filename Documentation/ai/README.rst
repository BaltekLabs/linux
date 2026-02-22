=====================================
Linux AI Subsystem - Architecture Guide
=====================================

:Author: Linux AI Project
:Version: 0.1

Overview
========

The Linux AI subsystem integrates native AI into the kernel, making the OS
AI-aware from the ground up. Rather than AI being a userspace application,
it is a first-class kernel citizen with direct access to all subsystems.

The design philosophy is **developer-first**: full root access, no
sandboxing, no confirmation dialogs by default. The AI is a trusted system
component on par with the kernel itself.

Architecture Layers
===================

::

  ┌─────────────────────────────────────────────────────────────────────┐
  │                      DTE (Developer Terminal Env)                    │
  │   tools/dte/   ncurses TUI  ·  tiled window mgr  ·  chat interface │
  └──────────────────────────┬──────────────────────────────────────────┘
                             │ Unix socket /run/aicore.sock
  ┌──────────────────────────▼──────────────────────────────────────────┐
  │                     aicore Daemon  (tools/aicore/)                   │
  │   LLM HTTP client  ·  OpenClaw executor  ·  conversation history    │
  │   supports: Anthropic Claude · Ollama · llama.cpp · LM Studio       │
  └───────────────┬─────────────────────────────────────────────────────┘
                  │ /dev/ai  (AI_MSG_* protocol)
  ┌───────────────▼─────────────────────────────────────────────────────┐
  │                  kernel/ai/ - AI Subsystem                           │
  │   ai_core.c: /dev/ai chardev · message routing · client tracking    │
  │   ai_proc.c: /proc/ai/ sysinfo · resources · stats                  │
  │   ai_claws.c: built-in tools · proc_list · mem_info · uptime        │
  └───────────────┬─────────────────────────────────────────────────────┘
                  │ ai_claw_register() / ai_claw_invoke()
  ┌───────────────▼─────────────────────────────────────────────────────┐
  │              OpenClaw Registry  (include/linux/ai.h)                 │
  │   proc_list · mem_info · sysctl_get · sysctl_set · dmesg_tail       │
  │   kernel modules can register additional claws dynamically           │
  └─────────────────────────────────────────────────────────────────────┘


Message Protocol (/dev/ai)
==========================

All communication uses ``struct ai_message`` (include/uapi/linux/ai.h):

.. code-block:: c

    struct ai_message {
        __u32 magic;      /* AI_MAGIC = 0x41490000 */
        __u32 type;       /* AI_MSG_QUERY, AI_MSG_RESPONSE, etc. */
        __u32 id;         /* Correlation ID */
        __u32 flags;      /* AI_FLAG_STREAM, AI_FLAG_SUDO, etc. */
        __u32 uid;        /* Sender UID (kernel-stamped) */
        __u32 pid;        /* Sender PID (kernel-stamped) */
        __u32 session;    /* Session ID (conversation continuity) */
        __u32 data_len;   /* Payload length */
        __u32 reserved[4];
        char  data[];     /* JSON payload */
    };

Message flow::

    Client         Kernel (/dev/ai)     aicore Daemon      LLM API
      │──write(query)──►│                   │                 │
      │                 │──route to daemon──►│                 │
      │                 │                   │──HTTP POST──────►│
      │                 │                   │                 │
      │                 │                   │◄── stream tok ──│
      │                 │◄── AI_MSG_STREAM ─│                 │
      │◄─ read(stream) ─│                   │                 │
      │                 │                   │◄── final resp ──│
      │                 │◄─ AI_MSG_RESPONSE ─│                │
      │◄─ read(resp) ───│                   │                 │


OpenClaw Tool Registry
======================

The **OpenClaw** system is the AI's "hands" - registered callable tools
that let the AI interact with every layer of the OS.

Kernel claws (registered with ``ai_claw_register()``)::

    proc_list       List running processes
    mem_info        Memory usage: total, free, available, swap
    uptime          System uptime and load averages
    kernel_version  Kernel release, arch, hostname

Userspace claws (in tools/aicore/tools.c)::

    shell_exec      Run any shell command as root
    file_read       Read any file
    file_write      Write any file
    file_list       Directory listing
    sysctl_get      Read kernel parameter
    sysctl_set      Write kernel parameter
    service_ctl     systemctl: start/stop/restart/status
    net_info        Network interfaces and addresses
    process_kill    Send signal to PID

Adding a new kernel claw::

    static int my_claw_execute(const char *params, char *result, size_t result_len)
    {
        return snprintf(result, result_len, "{\"value\":42}");
    }

    static struct ai_claw my_claw = {
        .name        = "my_subsystem_info",
        .description = "Get information from my kernel subsystem",
        .schema      = "{\"type\":\"object\",\"properties\":{}}",
        .execute     = my_claw_execute,
        .flags       = AI_CLAW_FLAG_READONLY | AI_CLAW_FLAG_KERNEL,
        .owner       = THIS_MODULE,
    };

    /* In module_init: */
    ai_claw_register(&my_claw);

    /* In module_exit: */
    ai_claw_unregister(&my_claw);


DTE - Developer Terminal Environment
=====================================

The DTE is a full-screen terminal AI chat interface with tiled window
management. It is not a traditional desktop - there are no windows in the
graphical sense. Instead, the terminal is split into panes:

Default layout (chat only)::

    ┌──────────────────────────────────────────────────────────────────┐
    │  AI Chat                                                       * │
    │                                                                  │
    │  DTE v0.1 - Linux AI Developer Terminal                         │
    │                                                                  │
    │  You: list running processes and show memory usage              │
    │                                                                  │
    │  AI: I'll check the running processes and memory for you.       │
    │  [calling proc_list...] [calling mem_info...]                   │
    │                                                                  │
    │  Top processes:                                                  │
    │    PID 1    init      S  prio=80                                 │
    │    PID 42   kworker   I  prio=60                                 │
    │                                                                  │
    │  Memory: 16GB total, 8.2GB available (51% free)                │
    │  Swap: 4GB total, 100MB used                                    │
    │                                                                  │
    ├──────────────────────────────────────────────────────────────────┤
    │ DTE v0.1 | connected                        F1=Help ^Q=Quit    │
    ├──────────────────────────────────────────────────────────────────┤
    │> _                                                               │
    └──────────────────────────────────────────────────────────────────┘

With side pane (Ctrl+S)::

    ┌─────────────────────────────────────┬──────────────────────────┐
    │  AI Chat                          * │  SysInfo                  │
    │  ...                               │  {                        │
    │                                    │  "uptime_seconds": 3600,  │
    │                                    │  "load_1min": 0,          │
    │                                    │  "memory_free_kb": 8192   │
    │                                    │  }                        │
    ├─────────────────────────────────────┴──────────────────────────┤
    │ DTE v0.1 | connected                            14:32:01       │
    ├────────────────────────────────────────────────────────────────┤
    │> _                                                              │
    └────────────────────────────────────────────────────────────────┘


Getting Started
===============

1. **Build the AI subsystem** (kernel config)::

       make menuconfig
       # Enable: Kernel Features -> Linux AI subsystem (OpenClaw native AI)

2. **Install aicore daemon**::

       cd tools/aicore
       make && sudo make install
       sudo cp config.json.example /etc/aicore/config.json
       # Edit /etc/aicore/config.json: set api_key and model

3. **Start aicore**::

       # Start manually:
       sudo aicore --config /etc/aicore/config.json

       # Or via systemd:
       sudo systemctl enable --now aicore

4. **Launch DTE**::

       dte

5. **Use local LLMs** (no API key needed)::

       # Install Ollama:
       curl -fsSL https://ollama.ai/install.sh | sh
       ollama pull llama3.2

       # Set config.json:
       # "backend": "ollama"
       # "api_url": "http://localhost:11434/api/chat"
       # "model": "llama3.2:latest"

       sudo aicore


Security Considerations
========================

The Linux AI subsystem runs with full root access by design. This is
intentional for the developer use case. For production deployments:

- Restrict ``/dev/ai`` permissions (default: 0666, change to 0600)
- Set ``tool_mode: "readonly"`` to prevent write operations
- Disable ``enable_shell: false`` to block arbitrary command execution
- Use ``enable_file_write: false`` and ``enable_sysctl_write: false``
- Run aicore as a dedicated user (not root) with specific capabilities
- The kernel stamps uid/pid on all messages - clients cannot spoof these

The OpenClaw system is explicitly designed to be extended and locked down
per deployment. The kernel-side registry provides a stable ABI for adding
new tools without kernel changes.


/proc/ai Interface
==================

When ``CONFIG_AI_PROC_INTERFACE=y``::

    /proc/ai/sysinfo     JSON: kernel version, arch, hostname, CPU count
    /proc/ai/resources   JSON: memory, swap, uptime, load averages
    /proc/ai/stats       JSON: AI subsystem message statistics

These are readable by any process and provide the AI's situational context.

Example::

    $ cat /proc/ai/resources
    {
      "memory_total_kb": 16384000,
      "memory_free_kb": 4096000,
      "memory_available_kb": 8192000,
      "memory_buffers_kb": 512000,
      "swap_total_kb": 4096000,
      "swap_free_kb": 3900000,
      "uptime_secs": 86400,
      "load_avg_1min": 0,
      "load_avg_5min": 0,
      "load_avg_15min": 0
    }
