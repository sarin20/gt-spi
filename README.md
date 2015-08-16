# gt-spi

*******READ THIS FIRST********
THIS SOFTWARE WORKS AT KERNEL LEVEL AND NOT TESTED WELL
YOU USE IT WITH YOUR FULL RESPONSIBILITY
*****************************

to build this module you need to patch your kernel as described here: https://github.com/blackswift/openwrt
then add package in menuconfig then build like any other kernek module:
openwrt# make package/kernel/gt-spi/compile V=99

echo "<clk mosi miso cs frequency> [bytes_num rep_cnt]" > /sys/kernel/debug/spi.control
example:
echo "21 22 24 19 1000 3 10" > /sys/kernel/debug/spi.control 
starts SPI conversation at
21 - SCK
22 - MOSI
24 - MISO
19 - CS
with 1 kHz frequency
3 bytes datagram (CS will up and down each 3 bytes)
and will repeat 10 times

it was checked with AD7683 that needs 24 clocks

bytes_num * rep_cnt should be less than PAGE_SIZE

to read and write use mmap and /sys/kernel/debug/spi.control
example:
```c
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdint.h>

#define PAGE_SIZE 4096
#define FILE_PATH "/sys/kernel/debug/spi.data"
#define MAP_SIZE 44

int main(void) {
	
	int configfd;
	printf("Opening ");
	printf(FILE_PATH);
	printf(" for mapping\n");
	configfd = open(FILE_PATH, O_RDWR);
	if(configfd < 0) {
		perror("open");
		return -1;
	}

	uint8_t * address = NULL;
	printf("Creating the mapping:\n");
	address = mmap(NULL, MAP_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, configfd, 0);
	if (address == MAP_FAILED) {
		perror("mmap");
		return -1;
	}	
	printf("Mapped. Printing %i bytes:\n", MAP_SIZE);
	int i;
	for (i = 0; i < MAP_SIZE; i++) {
		printf("%i\t\'%c\'\t0x%X\t%i\n", i, (address[i] < 0x20 ? 'X' : address[i]), address[i], address[i]);
	}
	printf("Done:\n");
	close(configfd);
	printf("File closed. Bye!\n");

	return 0;
}
```
