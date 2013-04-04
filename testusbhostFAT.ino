

/*
  Mega + USB storage + expansion RAM + funky status LED
 */

// Comment out xmem.h include if you do not have the library.
#include <xmem.h>
#include <avrpins.h>
#include <max3421e.h>
#include <usbhost.h>
#include <usb_ch9.h>
#include <avr/pgmspace.h>
#include <address.h>
#include <Usb.h>
#include <usbhub.h>
#include <masstorage.h>
#include <message.h>
#include <avr/interrupt.h>
#include <avr/io.h>
#include <PCpartition/PCPartition.h>
#include <Storage.h> // This silly include is needed to make the Arduino IDE happy.
#include <FAT/FAT.h>

int led = 13; // the pin that the LED is attached to
volatile int brightness = 0; // how bright the LED is
volatile int fadeAmount = 80; // how many points to fade the LED by
volatile uint8_t current_state = 1;
volatile uint32_t LEDnext_time; // fade timeout
volatile uint8_t last_state = 0;
volatile boolean fatready = false;
volatile boolean partsready = false;
volatile boolean notified = false;
volatile uint32_t HEAPnext_time; // when to print out next heap report
volatile boolean runtest = false;
volatile boolean usbon = false;
volatile uint32_t usbon_time;
volatile boolean change = false;
volatile boolean reportlvl = false;
int cpart = 0;
// Only used during discovery.
PCPartition *PT;
storage_t sto[MAX_PARTS];
part_t parts[MAX_PARTS];


static USB Usb;
USBHub *Hubs[MAX_HUBS];
PFAT *Fats[MAX_PARTS];

#define prescale1       ((1 << WGM12) | (1 << CS10))
#define prescale8       ((1 << WGM12) | (1 << CS11))
#define prescale64      ((1 << WGM12) | (1 << CS10) | (1 << CS11))
#define prescale256     ((1 << WGM12) | (1 << CS12))
#define prescale1024    ((1 << WGM12) | (1 << CS12) | (1 << CS10))

unsigned int getHeapend() {
        extern unsigned int __heap_start;

        if ((unsigned int)__brkval == 0) {
                return (unsigned int)&__heap_start;
        } else {
                return (unsigned int)__brkval;
        }
}

unsigned int freeHeap() {
        if (SP < (unsigned int)__malloc_heap_start) {
                return ((unsigned int)__malloc_heap_end - getHeapend());
        } else {
                return (SP - getHeapend());
        }
}

static int my_putc(char c, FILE *t) {
        Serial.write(c);
}

void setup() {
        // Set this to higher values to enable more debug information
        // minimum 0x00, maximum 0xff
        UsbDEBUGlvl = 0x7f;
        // declare pin 9 to be an output:
        pinMode(led, OUTPUT);
        pinMode(2, OUTPUT);
        // Initialize 'debug' serial port
        Serial.begin(115200);
        fdevopen(&my_putc, 0);

        // Blink pin 9:
        delay(500);
        analogWrite(led, 255);
        delay(500);
        analogWrite(led, 0);
        delay(500);
        analogWrite(led, 255);
        delay(500);
        analogWrite(led, 0);
        delay(500);
        analogWrite(led, 255);
        delay(500);
        analogWrite(led, 0);
        printf("\r\n\n\n\n\n\n\n\n\n\n\n\n\n\n\nStart\r\n");
        printf("Current UsbDEBUGlvl %02x\r\n", UsbDEBUGlvl);
        printf("'+' and '-' increase/decrease by 0x01\r\n");
        printf("'.' and ',' increase/decrease by 0x10\r\n");
        printf("'t' will run a 10MB write/read test and print out the time it took.\r\n");
        printf("'e' will toggle vbus off for a few moments.\r\n");
        analogWrite(led, 255);

        delay(500);
        analogWrite(led, 0);
        delay(500);

        delay(100);
        analogWrite(led, 255);
        delay(100);
        analogWrite(led, 0);
        LEDnext_time = millis() + 1;
#ifdef EXT_RAM
        xmem::begin(true);
        printf("Total EXT RAM banks %i\r\n", xmem::getTotalBanks());
#endif
        printf("Available heap: %u Bytes\r\n", freeHeap());
        printf("SP %x\r\n", (uint8_t *)(SP));

        // Even though I'm not going to actually be deleting,
        // I want to be able to have slightly more control.
        // Besides, it is easier to initialize stuff...

        for (int i = 0; i < MAX_HUBS; i++) {
                Bulk[i] = new BulkOnly(&Usb);
                printf("Available heap: %u Bytes\r\n", freeHeap());
                printf("SP %x\r\n", (uint8_t *)(SP));
        }
        while (Usb.Init() == -1) {
                printf("No\r\n");
                Notify(PSTR("OSC did not start."), 0x40);
        }
        // usb VBUS _OFF_
        Usb.gpioWr(0x00);
        digitalWrite(2, 0);
        usbon_time = millis() + 2000;
        cli();
        TCCR3A = 0;
        TCCR3B = 0;
        // (0.01/(1/((16 *(10^6)) / 8))) - 1
        OCR3A = 19999;
        TCCR3B |= prescale8;
        TIMSK3 |= (1 << OCIE1A);
        sei();

        HEAPnext_time = millis() + 10000;
}

ISR(TIMER3_COMPA_vect) {
        // Adjust UsbDEBUGlvl level on-the-fly.
        // + to increase, - to decrease, * to display current level.
        // . to increase by 16, , to decrease by 16
        // e to flick VBUS
        // * to report debug level
        if (Serial.available()) {
                int inByte = Serial.read();
                switch (inByte) {
                        case '+':
                                if (UsbDEBUGlvl < 0xff) UsbDEBUGlvl++;
                                reportlvl = true;
                                break;
                        case '-':
                                if (UsbDEBUGlvl > 0x00) UsbDEBUGlvl--;
                                reportlvl = true;
                                break;
                        case '.':
                                if (UsbDEBUGlvl < 0xf0) UsbDEBUGlvl += 16;
                                reportlvl = true;
                                break;
                        case ',':
                                if (UsbDEBUGlvl > 0x0f) UsbDEBUGlvl -= 16;
                                reportlvl = true;
                                break;
                        case '*':
                                reportlvl = true;
                                break;
                        case 't':
                                runtest = true;
                                break;
                        case 'e':
                                change = true;
                                usbon = false;
                                break;
                }
        }

        if (millis() >= LEDnext_time) {
                LEDnext_time = millis() + 30;

                // set the brightness of pin 9:
                analogWrite(led, brightness);

                // change the brightness for next time through the loop:
                brightness = brightness + fadeAmount;

                // reverse the direction of the fading at the ends of the fade:
                if (brightness <= 0) {
                        brightness = 0;
                        fadeAmount = -fadeAmount;
                }
                if (brightness >= 255) {
                        brightness = 255;
                        fadeAmount = -fadeAmount;
                }
        }
}

bool isfat(uint8_t t) {
        return (t == 0x01 || t == 0x04 || t == 0x06 || t == 0x0b || t == 0x0c || t == 0x0e || t == 0x1);
}

void die(FRESULT rc) {
        printf("Failed with rc=%u.\r\n", rc);
        //for (;;);

}

void loop() {
        if (reportlvl) {
                printf("Current UsbDEBUGlvl %02x\r\n", UsbDEBUGlvl);
                reportlvl = false;
        }
        // Print a heap status report about every 10 seconds.
        if (millis() >= HEAPnext_time) {
                if (UsbDEBUGlvl > 0x50) {
                        {
                        printf("Available heap: %u Bytes\r\n", freeHeap());
                        }
                }
                HEAPnext_time = millis() + 10000;
        }

        // Horrid! This sort of thing really belongs in an ISR, not here!
        // We also will be needing to test each hub port, we don't do this yet!
        if (!change && !usbon && millis() >= usbon_time) {
                change = true;
                usbon = true;
        }

        if (change) {
                change = false;
                if (usbon) {
                        printf("VBUS on\r\n");
                        Usb.gpioWr(0xFF);
                        digitalWrite(2, 1);
                } else {
                        Usb.gpioWr(0x00);
                        digitalWrite(2, 0);
                        usbon = false;
                        usbon_time = millis() + 2000;
                }
        }

        Usb.Task();
        current_state = Usb.getUsbTaskState();
        if (current_state != last_state) {
                if (UsbDEBUGlvl > 0x50)
                        printf("USB state = %x\r\n", current_state);
                if (current_state == USB_STATE_RUNNING) {
                        fadeAmount = 30;
                        /*
                        partsready = false;
                        for (int i = 0; i < cpart; i++) {
                                if (Fats[i] != NULL)
                                        delete Fats[i];
                        }
                        fatready = false;
                        notified = false;
                        cpart = 0;
                         */
                }
                if (current_state == USB_DETACHED_SUBSTATE_WAIT_FOR_DEVICE) {
                        fadeAmount = 80;
                        partsready = false;
                        for (int i = 0; i < cpart; i++) {
                                if (Fats[i] != NULL)
                                        delete Fats[i];
                        }
                        fatready = false;
                        notified = false;
                        cpart = 0;
                }
                if (current_state == 0xa0) {
                        printf("VBUS off\r\n");
                        // safe to do here
                        Usb.gpioWr(0x00);
                        digitalWrite(2, 0);
                        usbon = false;
                        usbon_time = millis() + 2000;
                        change = false;
                }
                last_state = current_state;
        }

        // only do any of this if usb is on
        if (usbon) {
                if (partsready && !fatready) {
                        if (cpart > 0) fatready = true;
                }

                // This is horrible, and needs to be moved elsewhere!
                // Also does not properly detect a single connect/disconnect on a hub yet.
                for (int B = 0; B < MAX_HUBS; B++) {
                        if (!partsready && Bulk[B]->GetAddress() != NULL) {
                                // Build a list.
                                int ML = Bulk[B]->GetbMaxLUN();
                                printf("MAXLUN =%i\r\n", ML);
                                ML++;
                                for (int i = 0; i < ML; i++) {
                                        volatile Capacity capacity;
                                        int rcode = Bulk[B]->ReadCapacity(i, sizeof (Capacity), (uint8_t*) & capacity);
                                        if (!rcode) {
                                                sto[i].private_data = &info[i];
                                                info[i].lun = i;
                                                info[i].B = B;
                                                info[i].volmap = 0; // TO-DO: keep track of > 1
                                                sto[i].Read = *PRead;
                                                sto[i].Write = *PWrite;
                                                sto[i].Reads = *PReads;
                                                sto[i].Writes = *PWrites;
                                                sto[i].TotalSectors = ((uint32_t)capacity.data[0] << 24) + ((uint32_t)capacity.data[1] << 16) + ((uint32_t)capacity.data[2] << 8) + (uint32_t)capacity.data[3];
                                                sto[i].SectorSize = ((uint16_t)capacity.data[6] << 8) + (uint16_t)capacity.data[7];
                                                printf("LUN:\t\t%u\r\n", i);
                                                printf("Total Sectors:\t%08lx\t%lu\r\n", sto[i].TotalSectors, sto[i].TotalSectors);
                                                printf("Sector Size:\t%04x\t\t%u\r\n", sto[i].SectorSize, sto[i].SectorSize);
                                                // get the partition data...
                                                PT = new PCPartition;

                                                if (!PT->Init(&sto[i])) {
                                                        part_t *apart;
                                                        for (int j = 0; j < 4; j++) {
                                                                apart = PT->GetPart(j);
                                                                if (apart != NULL && apart->type != 0x00) {
                                                                        memcpy(&(parts[cpart]), apart, sizeof (part_t));
                                                                        printf("Partition %u type %#02x\r\n", j, parts[cpart].type);
                                                                        // for now
                                                                        if (isfat(parts[cpart].type)) {
                                                                                Fats[cpart] = new PFAT;
                                                                                int r = Fats[cpart]->Init(&sto[i], cpart, parts[cpart].firstSector);
                                                                                if (r) {
                                                                                        delete Fats[cpart];
                                                                                } else cpart++;
                                                                        }
                                                                }
                                                        }
                                                } else {
                                                        // try superblock
                                                        Fats[cpart] = new PFAT;
                                                        int r = Fats[cpart]->Init(&sto[i], cpart, 0);
                                                        if (r) {
                                                                printf("Superblock error %x\r\n", r);
                                                                delete Fats[cpart];
                                                        } else cpart++;

                                                }
                                                delete PT;
                                        } else {
                                                sto[i].Read = NULL;
                                                sto[i].Write = NULL;
                                                sto[i].Writes = NULL;
                                                sto[i].Reads = NULL;
                                                sto[i].TotalSectors = 0UL;
                                                sto[i].SectorSize = 0UL;
                                        }
                                }
                                partsready = true;
                        }
                }

                if (fatready) {
                        if (!notified) {
                                fadeAmount = 5;
                                notified = true;
                                FIL Fil; /* File object */
                                BYTE Buff[128]; /* File read buffer */
                                FRESULT rc; /* Result code */
                                DIR dir; /* Directory object */
                                FILINFO fno; /* File information object */
                                UINT bw, br, i;
                                printf("\r\nOpen an existing file (message.txt).\r\n");
                                rc = f_open(&Fil, "0:/MESSAGE.TXT", FA_READ);
                                if (rc) printf("Error %i, message.txt not found.\r\n");
                                else {
                                        printf("\r\nType the file content.\r\n");
                                        for (;;) {
                                                rc = f_read(&Fil, Buff, sizeof Buff, &br); /* Read a chunk of file */
                                                if (rc || !br) break; /* Error or end of file */
                                                for (i = 0; i < br; i++) {
                                                        /* Type the data */
                                                        if (Buff[i] == '\n')
                                                                putchar('\r');
                                                        if (Buff[i] != '\r')
                                                                putchar(Buff[i]);
                                                }
                                        }
                                        if (rc) {
                                                die(rc);
                                                goto out;
                                        }

                                        printf("\r\nClose the file.\r\n");
                                        rc = f_close(&Fil);
                                        if (rc) goto out;
                                }
                                printf("\r\nCreate a new file (hello.txt).\r\n");
                                rc = f_open(&Fil, "0:/Hello.TxT", FA_WRITE | FA_CREATE_ALWAYS);
                                if (rc) goto out;

                                printf("\r\nWrite a text data. (Hello world!)\r\n");
                                rc = f_write(&Fil, "Hello world!\r\n", 14, &bw);
                                if (rc) goto out;
                                printf("%u bytes written.\r\n", bw);

                                printf("\r\nClose the file.\r\n");
                                rc = f_close(&Fil);
                                if (rc) goto out;

                                printf("\r\nOpen root directory.\r\n");
                                rc = f_opendir(&dir, "0:/");
                                if (rc) goto out;

                                printf("\r\nDirectory listing...\r\n");
                                for (;;) {
                                        rc = f_readdir(&dir, &fno); /* Read a directory item */
                                        if (rc || !fno.fname[0]) break; /* Error or end of dir */

                                        char att[] = "-rw---";
                                        if (fno.fattrib & AM_DIR) {
                                                att[0] = 'd';
                                        }
                                        if (fno.fattrib & AM_RDO) {
                                                att[2] = '-';
                                        }
                                        if (fno.fattrib & AM_HID) {
                                                att[3] = 'h';
                                        }
                                        if (fno.fattrib & AM_SYS) {
                                                att[4] = 's';
                                        }
                                        if (fno.fattrib & AM_ARC) {
                                                att[5] = 'a';
                                        }
                                        if (*fno.lfname)
                                                printf("%s %8lu  %s (%s)\r\n", att, fno.fsize, fno.fname, fno.lfname);
                                        else
                                                printf("%s %8lu  %s\r\n", att, fno.fsize, fno.fname);
                                }
out:
                                if (rc) die(rc);
                                printf("\r\nTest completed.\r\n");

                        }
                        if (runtest) {
                                runtest = false;
                                printf("\r\nCreate a new 10MB test file (10MB.bin).\r\n");
                                FIL Fil; /* File object */
                                BYTE Buff[512]; /* File read buffer */
                                FRESULT rc; /* Result code */
                                UINT br, bw, i;
                                ULONG wt, rt, start, end;
                                for (i = 0; i < 512; i++) Buff[i] = i & 0xff;
                                rc = f_open(&Fil, "0:/10MB.bin", FA_WRITE | FA_CREATE_ALWAYS);
                                if (rc) die(rc);
                                start = millis();
                                for (i = 0; i < 20480; i++) {
                                        rc = f_write(&Fil, &Buff, 512, &bw);
                                        if (rc) die(rc);
                                }
                                rc = f_close(&Fil);
                                if (rc) die(rc);
                                end = millis();
                                wt = end - start;
                                printf("Time to write 10485760 bytes: %lu ms (%lu sec) \r\n", wt, (500 + wt) / 1000UL);
                                rc = f_open(&Fil, "0:/10MB.bin", FA_READ);
                                start = millis();
                                if (rc) die(rc);
                                for (;;) {
                                        rc = f_read(&Fil, Buff, sizeof Buff, &br); /* Read a chunk of file */
                                        if (rc || !br) break; /* Error or end of file */
                                }
                                end = millis();
                                if (rc) die(rc);
                                rc = f_close(&Fil);
                                if (rc) die(rc);
                                rt = end - start;
                                printf("Time to read 10485760 bytes: %lu ms (%lu sec)\r\nDelete test file\r\n", rt, (500 + rt) / 1000UL);
                                rc = f_unlink("0:/10MB.bin");
                                if (rc) die(rc);
                                printf("10MB timing test finished.\r\n");
                        }
                }
        }
}
