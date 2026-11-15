// Process memory for the soak test (tests/test_soak.py, tests/native/soak_qa.inc). Its own
// translation unit, compiled only into halo-ac-qa: the Mach headers stay apart from cube.h.
#ifdef __APPLE__
#include <mach/mach.h>

// Resident size now and at its peak, and the physical footprint (what Activity Monitor
// shows as Memory: resident plus compressed, the figure that grows with a leak), in bytes.
bool haloqamemory(unsigned long long &resident, unsigned long long &peak, unsigned long long &footprint)
{
    mach_task_basic_info_data_t basic;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if(task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&basic), &count) != KERN_SUCCESS)
        return false;
    resident = basic.resident_size;
    peak = basic.resident_size_max;
    task_vm_info_data_t vm;
    count = TASK_VM_INFO_COUNT;
    footprint = task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &count) == KERN_SUCCESS
              ? vm.phys_footprint : 0;
    return true;
}
#else
// tests/tools/compile-check-linux.sh builds the QA client on Linux only to check that it links;
// the soak runs on macOS.
bool haloqamemory(unsigned long long &resident, unsigned long long &peak, unsigned long long &footprint)
{
    resident = peak = footprint = 0;
    return false;
}
#endif
