#include "nemu.h"
#include "monitor.h"
#include "device.h"
#include <elf.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>

vaddr_t uimage_base = UNMAPPED_BASE + DDR_BASE + 24 * 1024 * 1024;
void serial_enqueue_ascii(char);
uint32_t entry_start = 0xbfc00000;

char *elf_file = NULL;
char *symbol_file = NULL;
static char *img_file = NULL;
static char *kernel_img = NULL;

work_mode_t work_mode = MODE_GDB;

void bram_init(vaddr_t entry);

size_t get_file_size(const char *img_file) {
  struct stat file_status;
  lstat(img_file, &file_status);
  if(S_ISLNK(file_status.st_mode)) {
	char *buf = malloc(file_status.st_size);
	size_t size = readlink(img_file, buf, file_status.st_size);
	size = get_file_size(buf);
	free(buf);
	return size;
  } else {
	return file_status.st_size;
  }
}

void *read_file(const char *filename) {
  size_t size = get_file_size(filename);
  int fd = open(filename, O_RDONLY);
  if(fd == -1) return NULL;

  // malloc buf which should be freed by caller
  void *buf = malloc(size);
  int len = 0;
  while(len < size) {
	len += read(fd, buf, size - len);
  }
  return buf;
}

void load_elf() {
  Assert(elf_file, "Need an elf file");
  Log("The elf is %s", elf_file);

  /* set symbol file to elf_file */
  symbol_file = elf_file;
  const uint32_t elf_magic = 0x464c457f;

  void *buf = read_file(elf_file);
  Assert(buf, "elf file '%s' cannot be opened for read\n", elf_file);

  Elf32_Ehdr *elf = buf;

  uint32_t *p_magic = buf;
  assert(*p_magic == elf_magic);

  for(int i = 0; i < elf->e_phnum; i++) {
	  Elf32_Phdr *ph = (void*)buf + i * elf->e_phentsize + elf->e_phoff;
	  if(ph->p_type != PT_LOAD) { continue; }

	  void *p_vaddr = paddr_map(ph->p_vaddr, ph->p_memsz);
	  memcpy(p_vaddr, buf + ph->p_offset, ph->p_filesz); 
	  memset(p_vaddr + ph->p_filesz, 0, ph->p_memsz - ph->p_filesz);
  }

  entry_start = elf->e_entry;
  free(buf);
}

static inline void load_image(const char *img, vaddr_t vaddr) {
  Assert(img, "Need an image file");
  Log("The image is %s\n", img);

  size_t size = get_file_size(img);
  void *buf = read_file(img);
  void *paddr = paddr_map(vaddr, size);
  memcpy(paddr, buf, size);
  free(buf);
}

static inline void assume_elf_file() {
  /* assume img_file is xxx.bin and elf_file is xxx */
  char *end = strrchr(img_file, '.');
  if(end) {
	  *end = 0;
	  elf_file = img_file;
  }
}

void sigint_handler(int no) {
  nemu_state = NEMU_STOP;
}

static inline void parse_args(int argc, char *argv[]) {
  int o;
  while ( (o = getopt(argc, argv, "-S:bcdi:e:k:")) != -1) {
    switch (o) {
	  case 'S': symbol_file = optarg; break;
	  case 'k': kernel_img = optarg;
				load_image(kernel_img, uimage_base);
				break;
	  case 'd': work_mode |= MODE_DIFF; break;
      case 'b': work_mode |= MODE_BATCH; break;
      case 'c': work_mode |= MODE_LOG; break;
      case 'e':
                if (elf_file != NULL)
				  Log("too much argument '%s', ignored", optarg);
                else
				  elf_file = optarg;
                break;
      case 'i':
                if (img_file != NULL)
				  Log("too much argument '%s', ignored", optarg);
                else
				  img_file = optarg;
                break;
      default:
                panic("Usage: %s [-b] [-c] [-d] [-i img_file] [-k uImage] [-e elf_file]", argv[0]);
    }
  }
}

work_mode_t init_monitor(int argc, char *argv[]) {
  /* Perform some global initialization. */

  /* Parse arguments. */
  parse_args(argc, argv);

  /* Load the image to memory. */
  if(elf_file) {
	load_elf();
  } else {
	load_image(img_file, entry_start);
  }

  if(!(work_mode & MODE_BATCH))
	signal(SIGINT, sigint_handler);

  // send command to uboot
  char cmd[1024];
  sprintf(cmd, "bootm 0x%08x - 0xbfc3b730\n", uimage_base);
  for(char *p = cmd; *p; p++)
	serial_enqueue_ascii(*p);

  /* Initialize this virtual computer system. */
  bram_init(entry_start);
  init_cpu(entry_start);

  return work_mode;
}
