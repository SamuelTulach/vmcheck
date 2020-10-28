#include <ntifs.h>
#include <intrin.h>

#define Log(x, ...) DbgPrintEx(0, 0, "[vmcheck] " x, __VA_ARGS__)

#define IA32_MPERF_MSR 0x000000E7
#define IA32_APERF_MSR 0x000000E8
#define	IA32_TIME_STAMP_COUNTER 0x00000010

typedef struct _TestResults
{
	DWORD64 RdtscCalculated;
	BOOLEAN RdtscFail;
	DWORD64 TimestampCalculated;
	BOOLEAN TimestampFail;
	DWORD64 AperfCalculated;
	BOOLEAN AperfFail;
} TestResults;
TestResults testResults;

void RdtscTiming()
{
	//
	// Get first TSC value
	//

	DWORD64 calibration = __rdtsc();

	//
	// Wait for one second (not precise, should be improved)
	//
	
	LARGE_INTEGER waitTime;
	waitTime.QuadPart = -10000000; // 1 second
	KeDelayExecutionThread(KernelMode, FALSE, &waitTime);

	//
	// Save how much ticks has passed in one second
	// aka TSC frequency without vm-exits
	//
	
	calibration = __rdtsc() - calibration;

	//
	// Loop multiple times counting how many ticks pass
	// with CPUID in between (CPUID causes unconditional exit)
	// and save total time it took
	//
	
	DWORD64 total = 0;
	for (int i = 0; i < 25000; i++)
	{
		DWORD64 start = __rdtsc();

		int data[4];
		__cpuid(data, 0);
		
		total += __rdtsc() - start;
	}

	//
	// Now we can simply compare those saved values
	// since we know how fast is the timer without CPUID,
	// we can then safely say if the CPUID time is just
	// ridiculously high or not
	//

	DWORD64 calculated = 100000 * total / calibration;
	testResults.RdtscCalculated = calculated;
	testResults.RdtscFail = (calculated > 200);
}


void TimestampTiming()
{
	//
	// This test is basically same as with RDTSC with the only difference
	// being reading the value directly from MSR
	// Some hypervisors have this implemented pretty well so you don't really have
	// to do anything
	//

	//
	// Read first TSC value from MSR
	//

	DWORD64 calibration = __readmsr(IA32_TIME_STAMP_COUNTER);

	//
	// Wait for one second (not precise, should be improved)
	//

	LARGE_INTEGER waitTime;
	waitTime.QuadPart = -10000000; // 1 second
	KeDelayExecutionThread(KernelMode, FALSE, &waitTime);

	//
	// Save how much ticks has passed in one second
	// aka TSC frequency without vm-exits
	//

	DWORD64 end = __readmsr(IA32_TIME_STAMP_COUNTER);
	calibration = end - calibration;

	//
	// Exactly the same just different read
	//

	DWORD64 total = 0;
	for (int i = 0; i < 25000; i++)
	{
		DWORD64 start = __readmsr(IA32_TIME_STAMP_COUNTER);

		int data[4];
		__cpuid(data, 0);

		end = __readmsr(IA32_TIME_STAMP_COUNTER);
		total += end - start;
	}

	//
	// Again the same as with RDTSC
	//

	DWORD64 calculated = 100000 * total / calibration;
	testResults.TimestampCalculated = calculated;
	testResults.TimestampFail = (calculated > 300);
}

void AperfTiming()
{
	//
	// Read APERF counter which only ticks when CPU is in CR0 state
	// aka when there is something being executed
	// Most hypervisors (such as KVM) does not even implement this MSR
	// so it will always just return 0 or same value
	//
	// From Intel documentation:
	// IA32_APERF MSR (E8H) increments in proportion to actual performance,
	// while accounting for hardware coordi-nation of P-state and TM1/TM2;
	// or software initiated throttling
	//
	
	DWORD64 start = __readmsr(IA32_APERF_MSR) << 32;

	//
	// Call CPUID to cause vm-exit
	//
	
	int data[4];
	__cpuid(data, 1);

	//
	// Get the difference
	//
	
	DWORD64 end = __readmsr(IA32_APERF_MSR) << 32;

	//
	// Check if the counter actually added something
	// On real machine with turbo (reps. not static clock speed)
	// this value should be pretty high
	//
	
	DWORD64 calculated = end - start;
	testResults.AperfCalculated = calculated;
	testResults.AperfFail = (calculated < 10000);
}

void PerformTests()
{
	//
	// Force running of the test on single core
	//

	const ULONG numberOfProcessors = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
	PROCESSOR_NUMBER processorNumber;

	NTSTATUS status = KeGetProcessorNumberFromIndex(numberOfProcessors - 1, &processorNumber);
	if (!NT_SUCCESS(status))
	{
		Log("Failed to get processor number!\n");
		return;
	}

	GROUP_AFFINITY affinity, oldAffinity;
	affinity.Group = processorNumber.Group;
	affinity.Mask = 1ULL << processorNumber.Number;
	affinity.Reserved[0] = affinity.Reserved[1] = affinity.Reserved[2] = 0;
	KeSetSystemGroupAffinityThread(&affinity, &oldAffinity);

	//
	// Raise IRQL and disable interrupts since without it
	// we might see lot of spikes in the tests
	//
	
	KIRQL originalIrql;
	KeRaiseIrql(HIGH_LEVEL, &originalIrql);
	_disable();

	//
	// Perform individual tests one by one
	//
	RdtscTiming();
	TimestampTiming();
	AperfTiming();

	//
	// Reenable interrupts, lower IRQL and rever execution to original
	// core
	//
	
	_enable();
	KeLowerIrql(originalIrql);
	KeRevertToUserGroupAffinityThread(&oldAffinity);
}

void PrintResults()
{
	Log("RDTSC with CPUID: %llu (%s)\n", testResults.RdtscCalculated, testResults.RdtscFail ? "fail" : "ok");
	Log("MSR TIMESTAMP with CPUID: %llu (%s)\n", testResults.TimestampCalculated, testResults.TimestampFail ? "fail" : "ok");
	Log("MSR APERF with CPUID: %llu (%s)\n", testResults.AperfCalculated, testResults.AperfFail ? "fail" : "ok");
}

NTSTATUS DriverEntry(void* dummy1, void* dummy2)
{
	UNREFERENCED_PARAMETER(dummy1);
	UNREFERENCED_PARAMETER(dummy2);

	Log("Loaded. Build on %s.\n", __DATE__);

	PerformTests();
	PrintResults();

	return STATUS_SUCCESS;
}