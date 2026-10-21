/*
 * g4temp: read the MAX6642 temperature sensor of a Mac mini G4 (PowerMac10,1/10,2)
 * under Mac OS X 10.4/10.5, through Apple's own I2C user client.
 *
 * The sensor is the "temp-monitor" node (compatible "max6642") on the PMU I2C bus;
 * its "reg" property is (bus << 8) | 8-bit address, e.g. 0x190 = bus 1, 0x90.
 * The parent PPCI2CInterface publishes IOUserClientClass = I2CUserClient, whose
 * read method (selector 2, struct in / struct out) is documented in Apple's
 * IOI2CFamily (IOI2CDefs.h). Only reads are issued, nothing is ever written.
 *
 * Needs root: the user client refuses unprivileged opens.
 *
 * Usage: g4temp        human-readable
 *        g4temp -p     Prometheus text format (node_hwmon_* like Linux hwmon)
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

/* IOI2CDefs.h: IOI2CUserClient methods and the read structures. Lock/Unlock are
 * the user client's open/close: without Lock every read is kIOReturnNotReady. */
enum { kI2CUCLock = 0, kI2CUCUnlock = 1, kI2CUCRead = 2 };
enum { kI2CMode_Combined = 3 };   /* write subaddress, repeated start, read */
#define kI2CUCBufSize 32

typedef struct {
	UInt8		mode;
	UInt8		busNo;
	UInt8		addr;		/* 8-bit address, R/W bit ignored */
	UInt8		subAddr;	/* register */
	IOByteCount	count;
} I2CReadInput;

typedef struct {
	IOByteCount	realCount;
	UInt8		buf[kI2CUCBufSize];
} I2CReadOutput;

/* MAX6642 registers (datasheet; same as Linux drivers/hwmon/max6642.c). */
#define REG_LOCAL_TEMP		0x00
#define REG_REMOTE_TEMP		0x01
#define REG_STATUS		0x02
#define REG_CONFIG		0x03
#define REG_LOCAL_HIGH		0x05
#define REG_REMOTE_HIGH		0x07
#define REG_REMOTE_TEMPL	0x10	/* bits 7:6 = 0.25 °C steps */
#define REG_LOCAL_TEMPL		0x11
#define REG_MAN_ID		0xFE
#define MAXIM_ID		0x4D

/* Status bits: 6 local high, 4 remote high, 2 remote diode open (fault). */
#define ST_BUSY		0x80
#define ST_LHIGH	0x40
#define ST_RHIGH	0x10
#define ST_OPEN		0x04

static io_connect_t conn;
static UInt8 bus, addr;

static int rd(UInt8 reg, UInt8 *val)
{
	I2CReadInput in;
	I2CReadOutput out;
	IOByteCount outSize = sizeof(out);
	kern_return_t kr;

	memset(&in, 0, sizeof(in));
	in.mode = kI2CMode_Combined;
	in.busNo = bus;
	in.addr = addr;
	in.subAddr = reg;
	in.count = 1;
	kr = IOConnectMethodStructureIStructureO(conn, kI2CUCRead, sizeof(in), &outSize, &in, &out);
	if (kr != KERN_SUCCESS || out.realCount != 1)
		return -1;
	*val = out.buf[0];
	return 0;
}

static void close_i2c(void)
{
	IOConnectMethodScalarIScalarO(conn, kI2CUCUnlock, 0, 0);
	IOServiceClose(conn);
}

static int find_sensor(io_registry_entry_t *bus_svc)
{
	io_iterator_t it;
	io_registry_entry_t e;
	CFDataRef reg;
	const UInt8 *p;
	int found = 0;

	if (IOServiceGetMatchingServices(kIOMasterPortDefault, IOServiceNameMatching("temp-monitor"), &it) != KERN_SUCCESS)
		return -1;
	while (!found && (e = IOIteratorNext(it))) {
		if (IOObjectConformsTo(e, "IOService") &&
		    (reg = IORegistryEntryCreateCFProperty(e, CFSTR("reg"), kCFAllocatorDefault, 0))) {
			if (CFGetTypeID(reg) == CFDataGetTypeID() && CFDataGetLength(reg) >= 4) {
				p = CFDataGetBytePtr(reg);	/* big-endian 32-bit */
				bus = p[2];
				addr = p[3];
				found = IORegistryEntryGetParentEntry(e, kIOServicePlane, bus_svc) == KERN_SUCCESS;
			}
			CFRelease(reg);
		}
		IOObjectRelease(e);
	}
	IOObjectRelease(it);
	return found ? 0 : -1;
}

static double temp(UInt8 hi, UInt8 lo)
{
	return (signed char)hi + ((lo >> 6) & 3) * 0.25;
}

int main(int argc, char **argv)
{
	io_registry_entry_t svc;
	kern_return_t kr;
	UInt8 id, st, lt, ltl, rt, rtl, lh, rh;
	double tl, tr;
	int prom = argc > 1 && !strcmp(argv[1], "-p");

	if (argc > 1 && !prom) {
		fprintf(stderr, "usage: %s [-p]\n", argv[0]);
		return 2;
	}
	if (geteuid() != 0) {
		fprintf(stderr, "g4temp: must run as root (the I2C user client refuses other users)\n");
		return 1;
	}
	if (find_sensor(&svc)) {
		fprintf(stderr, "g4temp: no temp-monitor (max6642) node in the I/O Registry\n");
		return 1;
	}
	kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
	IOObjectRelease(svc);
	if (kr == KERN_SUCCESS && (kr = IOConnectMethodScalarIScalarO(conn, kI2CUCLock, 0, 0)) != KERN_SUCCESS)
		IOServiceClose(conn);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "g4temp: cannot open the I2C user client: 0x%x\n", kr);
		return 1;
	}
	id = 0;
	if (rd(REG_MAN_ID, &id) || id != MAXIM_ID) {
		fprintf(stderr, "g4temp: bus %u addr 0x%02x is not a MAX6642 (manufacturer id 0x%02x)\n", bus, addr, id);
		close_i2c();
		return 1;
	}
	/* Read the low byte first: as in the Linux driver, the pair is then consistent. */
	if (rd(REG_LOCAL_TEMPL, &ltl) || rd(REG_LOCAL_TEMP, &lt) ||
	    rd(REG_REMOTE_TEMPL, &rtl) || rd(REG_REMOTE_TEMP, &rt) ||
	    rd(REG_LOCAL_HIGH, &lh) || rd(REG_REMOTE_HIGH, &rh) || rd(REG_STATUS, &st)) {
		fprintf(stderr, "g4temp: I2C read failed\n");
		close_i2c();
		return 1;
	}
	close_i2c();
	tl = temp(lt, ltl);
	tr = temp(rt, rtl);

	if (prom) {
		printf("# TYPE node_hwmon_chip_names gauge\n");
		printf("node_hwmon_chip_names{chip=\"max6642\",chip_name=\"max6642\"} 1\n");
		printf("# TYPE node_hwmon_sensor_label gauge\n");
		printf("node_hwmon_sensor_label{chip=\"max6642\",label=\"local\",sensor=\"temp1\"} 1\n");
		printf("node_hwmon_sensor_label{chip=\"max6642\",label=\"remote\",sensor=\"temp2\"} 1\n");
		printf("# TYPE node_hwmon_temp_celsius gauge\n");
		printf("node_hwmon_temp_celsius{chip=\"max6642\",sensor=\"temp1\"} %.2f\n", tl);
		printf("node_hwmon_temp_celsius{chip=\"max6642\",sensor=\"temp2\"} %.2f\n", tr);
		printf("# TYPE node_hwmon_temp_max_celsius gauge\n");
		printf("node_hwmon_temp_max_celsius{chip=\"max6642\",sensor=\"temp1\"} %d\n", (signed char)lh);
		printf("node_hwmon_temp_max_celsius{chip=\"max6642\",sensor=\"temp2\"} %d\n", (signed char)rh);
		printf("# TYPE node_hwmon_temp_max_alarm gauge\n");
		printf("node_hwmon_temp_max_alarm{chip=\"max6642\",sensor=\"temp1\"} %d\n", !!(st & ST_LHIGH));
		printf("node_hwmon_temp_max_alarm{chip=\"max6642\",sensor=\"temp2\"} %d\n", !!(st & ST_RHIGH));
		printf("# TYPE node_hwmon_temp_fault gauge\n");
		printf("node_hwmon_temp_fault{chip=\"max6642\",sensor=\"temp2\"} %d\n", !!(st & ST_OPEN));
	} else {
		printf("MAX6642 on PMU I2C bus %u, address 0x%02x\n", bus, addr);
		printf("local  (temp1, chip on board): %6.2f C  limit %3d C  high alarm: %s\n",
		    tl, (signed char)lh, st & ST_LHIGH ? "YES" : "no");
		printf("remote (temp2, external diode): %6.2f C  limit %3d C  high alarm: %s  diode: %s\n",
		    tr, (signed char)rh, st & ST_RHIGH ? "YES" : "no", st & ST_OPEN ? "OPEN" : "ok");
		printf("status 0x%02x%s\n", st, st & ST_BUSY ? " (conversion in progress)" : "");
	}
	return 0;
}
