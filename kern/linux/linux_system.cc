/*
 * Copyright (c) 2003 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "base/loader/aout_object.hh"
#include "base/loader/elf_object.hh"
#include "base/loader/object_file.hh"
#include "base/loader/symtab.hh"
#include "base/remote_gdb.hh"
#include "base/trace.hh"
#include "cpu/exec_context.hh"
#include "cpu/base_cpu.hh"
#include "kern/linux/linux_events.hh"
#include "kern/linux/linux_system.hh"
#include "mem/functional_mem/memory_control.hh"
#include "mem/functional_mem/physical_memory.hh"
#include "sim/builder.hh"
#include "dev/platform.hh"
#include "targetarch/isa_traits.hh"
#include "targetarch/vtophys.hh"

extern SymbolTable *debugSymbolTable;

using namespace std;

LinuxSystem::LinuxSystem(const string _name, const uint64_t _init_param,
                         MemoryController *_memCtrl, PhysicalMemory *_physmem,
                         const string &kernel_path, const string &console_path,
                         const string &palcode, const string &boot_osflags,
                         const bool _bin, const vector<string> &_binned_fns)
     : System(_name, _init_param, _memCtrl, _physmem, _bin, _binned_fns),
       bin(_bin), binned_fns(_binned_fns)
{
    kernelSymtab = new SymbolTable;
    consoleSymtab = new SymbolTable;

    // Load kernel code
    ObjectFile *kernel = createObjectFile(kernel_path);
    if (kernel == NULL)
        fatal("Could not load kernel file %s", kernel_path);

    // Load Console Code
    ObjectFile *console = createObjectFile(console_path);
    if (console == NULL)
        fatal("Could not load console file %s", console_path);

    // Load pal file
    ObjectFile *pal = createObjectFile(palcode);
    if (pal == NULL)
        fatal("Could not load PALcode file %s", palcode);
    pal->loadSections(physmem, true);

    // Load console file
    console->loadSections(physmem, true);

    // Load kernel file
    kernel->loadSections(physmem, true);
    kernelStart = kernel->textBase();
    kernelEnd = kernel->bssBase() + kernel->bssSize();
    kernelEntry = kernel->entryPoint();

    // load symbols
    if (!kernel->loadGlobalSymbols(kernelSymtab))
        panic("could not load kernel symbols\n");
        debugSymbolTable = kernelSymtab;

    if (!kernel->loadLocalSymbols(kernelSymtab))
        panic("could not load kernel local symbols\n");

    if (!console->loadGlobalSymbols(consoleSymtab))
        panic("could not load console symbols\n");

    DPRINTF(Loader, "Kernel start = %#x\n"
            "Kernel end   = %#x\n"
            "Kernel entry = %#x\n",
            kernelStart, kernelEnd, kernelEntry);

    DPRINTF(Loader, "Kernel loaded...\n");


#ifdef DEBUG
    kernelPanicEvent = new BreakPCEvent(&pcEventQueue, "kernel panic");
    consolePanicEvent = new BreakPCEvent(&pcEventQueue, "console panic");
#endif

    skipIdeDelay50msEvent = new LinuxSkipIdeDelay50msEvent(&pcEventQueue,
                                                     "ide_delay_50ms");

    skipDelayLoopEvent = new LinuxSkipDelayLoopEvent(&pcEventQueue,
                                                     "calibrate_delay");

    skipCacheProbeEvent = new LinuxSkipFuncEvent(&pcEventQueue, "determine_cpu_caches");

    Addr addr = 0;

    if (kernelSymtab->findAddress("est_cycle_freq", addr)) {
        Addr paddr = vtophys(physmem, addr);
        uint8_t *est_cycle_frequency =
            physmem->dma_addr(paddr, sizeof(uint64_t));

        if (est_cycle_frequency)
            *(uint64_t *)est_cycle_frequency = ticksPerSecond;
    }


    if (consoleSymtab->findAddress("env_booted_osflags", addr)) {
        Addr paddr = vtophys(physmem, addr);
        char *osflags = (char *)physmem->dma_addr(paddr, sizeof(uint32_t));

        if (osflags)
              strcpy(osflags, boot_osflags.c_str());
    }

    {
        Addr paddr = vtophys(physmem, PARAM_ADDR);
        char *commandline = (char*)physmem->dma_addr(paddr, sizeof(uint64_t));
        if (commandline)
            strcpy(commandline, boot_osflags.c_str());
    }


    if (consoleSymtab->findAddress("xxm_rpb", addr)) {
        Addr paddr = vtophys(physmem, addr);
        char *hwprb = (char *)physmem->dma_addr(paddr, sizeof(uint64_t));

        if (hwprb) {
            *(uint64_t*)(hwprb+0x50) = 34;      // Tsunami
            *(uint64_t*)(hwprb+0x58) = (1<<10);
        }
        else
            panic("could not translate hwprb addr to set system type/variation\n");

    } else
        panic("could not find hwprb to set system type/variation\n");

#ifdef DEBUG
    if (kernelSymtab->findAddress("panic", addr))
        kernelPanicEvent->schedule(addr);
    else
        panic("could not find kernel symbol \'panic\'");

    if (consoleSymtab->findAddress("panic", addr))
        consolePanicEvent->schedule(addr);
#endif

    if (kernelSymtab->findAddress("ide_delay_50ms", addr))
        skipIdeDelay50msEvent->schedule(addr+8);

    if (kernelSymtab->findAddress("calibrate_delay", addr))
        skipDelayLoopEvent->schedule(addr+8);

    if (kernelSymtab->findAddress("determine_cpu_caches", addr))
        skipCacheProbeEvent->schedule(addr+8);
}

LinuxSystem::~LinuxSystem()
{
    delete kernel;
    delete console;

    delete kernelSymtab;
    delete consoleSymtab;

    delete kernelPanicEvent;
    delete consolePanicEvent;
    delete skipIdeDelay50msEvent;
    delete skipDelayLoopEvent;
    delete skipCacheProbeEvent;
}


void
LinuxSystem::setDelayLoop(ExecContext *xc)
{
    Addr addr = 0;
    if (kernelSymtab->findAddress("loops_per_jiffy", addr)) {
        Addr paddr = vtophys(physmem, addr);

        uint8_t *loops_per_jiffy =
            physmem->dma_addr(paddr, sizeof(uint32_t));

        Tick cpuFreq = xc->cpu->getFreq();
        Tick intrFreq = platform->interrupt_frequency;
        *(uint32_t *)loops_per_jiffy =
            (uint32_t)((cpuFreq / intrFreq) * 0.9988);
    }
}

int
LinuxSystem::registerExecContext(ExecContext *xc)
{
    int xcIndex = System::registerExecContext(xc);

    if (xcIndex == 0) {
        // activate with zero delay so that we start ticking right
        // away on cycle 0
        xc->activate(0);
    }

    RemoteGDB *rgdb = new RemoteGDB(this, xc);
    GDBListener *gdbl = new GDBListener(rgdb, 7000 + xcIndex);
    gdbl->listen();
//    gdbl->accept();

    if (remoteGDB.size() <= xcIndex) {
        remoteGDB.resize(xcIndex+1);
    }

    remoteGDB[xcIndex] = rgdb;

    return xcIndex;
}


void
LinuxSystem::replaceExecContext(ExecContext *xc, int xcIndex)
{
    System::replaceExecContext(xcIndex, xc);
    remoteGDB[xcIndex]->replaceExecContext(xc);
}

bool
LinuxSystem::breakpoint()
{
    return remoteGDB[0]->trap(ALPHA_KENTRY_IF);
}

BEGIN_DECLARE_SIM_OBJECT_PARAMS(LinuxSystem)

    Param<bool> bin;
    SimObjectParam<MemoryController *> mem_ctl;
    SimObjectParam<PhysicalMemory *> physmem;
    Param<uint64_t> init_param;

    Param<string> kernel_code;
    Param<string> console_code;
    Param<string> pal_code;
    Param<string> boot_osflags;
    VectorParam<string> binned_fns;

END_DECLARE_SIM_OBJECT_PARAMS(LinuxSystem)

BEGIN_INIT_SIM_OBJECT_PARAMS(LinuxSystem)


    INIT_PARAM_DFLT(bin, "is this system to be binned", false),
    INIT_PARAM(mem_ctl, "memory controller"),
    INIT_PARAM(physmem, "phsyical memory"),
    INIT_PARAM_DFLT(init_param, "numerical value to pass into simulator", 0),
    INIT_PARAM(kernel_code, "file that contains the code"),
    INIT_PARAM(console_code, "file that contains the console code"),
    INIT_PARAM(pal_code, "file that contains palcode"),
    INIT_PARAM_DFLT(boot_osflags, "flags to pass to the kernel during boot",
                                   "a"),
    INIT_PARAM(binned_fns, "functions to be broken down and binned")


END_INIT_SIM_OBJECT_PARAMS(LinuxSystem)

CREATE_SIM_OBJECT(LinuxSystem)
{
    LinuxSystem *sys = new LinuxSystem(getInstanceName(), init_param, mem_ctl,
                                       physmem, kernel_code, console_code,
                                       pal_code, boot_osflags, bin, binned_fns);

    return sys;
}

REGISTER_SIM_OBJECT("LinuxSystem", LinuxSystem)
