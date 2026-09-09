/**
 * @file uac2_asmp_sup.c
 * @brief ASMP supervisor: ROMFS mount + sub-core 1 formatter boot.
 *
 * Mirrors the SDK examples/asmp supervisor flow (romdisk -> /romfs mount
 * -> mptask boot). Without CONFIG_EXAMPLES_UAC2_DAC_ASMP this is a stub
 * that reports unavailable (monitor falls back to local formatting).
 */

#include <nuttx/config.h>

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>

#ifdef CONFIG_EXAMPLES_UAC2_DAC_ASMP

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>

#include <nuttx/drivers/ramdisk.h>

#include <asmp/asmp.h>
#include <asmp/mptask.h>
#include <asmp/mpshm.h>
#include <asmp/mpmutex.h>

#include "worker/romfs.h"
#include "uac2_asmp.h"

#define SECTORSIZE   512
#define NSECTORS(b)  (((b) + SECTORSIZE - 1) / SECTORSIZE)

static mptask_t s_task;
static mpmutex_t s_mutex;
static mpshm_t s_shm;
static volatile struct uac2_asmp_shm_s *s_shared = NULL;
static bool s_booted = false;

int uac2_asmp_boot(volatile struct uac2_asmp_shm_s **out)
{
  char path[64];
  int ret;

  if (out == NULL)
    {
      return -1;
    }
  *out = NULL;

  if (s_booted && s_shared != NULL)
    {
      *out = s_shared;
      return 0;
    }

  /* 1. ROMFS with the worker ELF (generated into worker/romfs.h). */
  {
    struct stat st;
    if (stat(UAC2_ASMP_MOUNTPT, &st) < 0)
      {
        ret = romdisk_register(0, (FAR uint8_t *)romfs_img,
                               NSECTORS(romfs_img_len), SECTORSIZE);
        if (ret < 0)
          {
            printf("[UAC2-ASMP] romdisk_register failed: %d\n", ret);
            return -1;
          }
        ret = mount("/dev/ram0", UAC2_ASMP_MOUNTPT, "romfs",
                    MS_RDONLY, NULL);
        if (ret < 0)
          {
            printf("[UAC2-ASMP] mount romfs failed: %d\n", errno);
            return -1;
          }
      }
  }

  snprintf(path, sizeof(path), "%s/%s", UAC2_ASMP_MOUNTPT,
           UAC2_ASMP_WORKER);

  /* 2. Shared segment + ABI header (worker ABI-gates on these). */
  ret = mpshm_init(&s_shm, UAC2_ASMP_KEY_SHM,
                   sizeof(struct uac2_asmp_shm_s));
  if (ret < 0)
    {
      printf("[UAC2-ASMP] mpshm_init failed: %d\n", ret);
      return -1;
    }

  s_shared = (volatile struct uac2_asmp_shm_s *)mpshm_attach(&s_shm, 0);
  if (s_shared == NULL)
    {
      printf("[UAC2-ASMP] mpshm_attach failed\n");
      mpshm_destroy(&s_shm);
      return -1;
    }
  memset((void *)s_shared, 0, sizeof(struct uac2_asmp_shm_s));
  s_shared->abi_magic = UAC2_ASMP_MAGIC;
  s_shared->abi_version = UAC2_ASMP_VERSION;
  s_shared->abi_size = sizeof(struct uac2_asmp_shm_s);

  ret = mpmutex_init(&s_mutex, UAC2_ASMP_KEY_MUTEX);
  if (ret < 0)
    {
      printf("[UAC2-ASMP] mpmutex_init failed: %d\n", ret);
      mpshm_detach(&s_shm);
      mpshm_destroy(&s_shm);
      s_shared = NULL;
      return -1;
    }

  /* 3. Boot sub-core 1. */
  ret = mptask_init(&s_task, path);
  if (ret != 0)
    {
      printf("[UAC2-ASMP] mptask_init(%s) failed: %d\n", path, ret);
      goto fail;
    }
  ret = mptask_assign(&s_task);
  if (ret != 0)
    {
      printf("[UAC2-ASMP] mptask_assign failed: %d\n", ret);
      goto fail;
    }
  ret = mptask_bindobj(&s_task, &s_mutex);
  if (ret < 0)
    {
      printf("[UAC2-ASMP] bindobj(mutex) failed: %d\n", ret);
      goto fail;
    }
  ret = mptask_bindobj(&s_task, &s_shm);
  if (ret < 0)
    {
      printf("[UAC2-ASMP] bindobj(shm) failed: %d\n", ret);
      goto fail;
    }
  ret = mptask_exec(&s_task);
  if (ret < 0)
    {
      printf("[UAC2-ASMP] mptask_exec failed: %d\n", ret);
      goto fail;
    }

  /* 4. Wait for worker ready (ABI passed on sub-core). */
  for (int i = 0; i < 300; i++)
    {
      if (s_shared->worker_ready)
        {
          break;
        }
      usleep(10000);
    }
  if (!s_shared->worker_ready)
    {
      printf("[UAC2-ASMP] worker ready timeout (stage=0x%08lx hb=%lu)\n",
             (unsigned long)s_shared->dbg_stage,
             (unsigned long)s_shared->worker_hb);
      goto fail;
    }

  printf("[UAC2-ASMP] sub-core formatter online (monw)\n");
  fflush(stdout);
  s_booted = true;
  *out = s_shared;
  return 0;

fail:
  mpmutex_destroy(&s_mutex);
  mpshm_detach(&s_shm);
  mpshm_destroy(&s_shm);
  s_shared = NULL;
  return -1;
}

#else /* !CONFIG_EXAMPLES_UAC2_DAC_ASMP */

#include "uac2_asmp.h"

int uac2_asmp_boot(volatile struct uac2_asmp_shm_s **out)
{
  if (out != NULL)
    {
      *out = NULL;
    }
  return -1;
}

#endif /* CONFIG_EXAMPLES_UAC2_DAC_ASMP */
