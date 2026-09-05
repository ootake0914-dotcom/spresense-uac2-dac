/**
 * @file uac2_audio_dma.c
 * @brief CXD5247 / S-Master Audio DMA Playback Bridge (192kHz/24bit)
 *
 * Implements high-resolution audio streaming from USB Isochronous ring buffer
 * to the Sony Spresense hardware audio subsystem (/dev/pcm0) using NuttX
 * standard audio APB (audio packet buffer) DMA queuing.
 */

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <mqueue.h>

#include <nuttx/audio/audio.h>
#include <arch/board/board.h>
#include <arch/chip/audio.h>

#include "uac2.h"
#include "uac2_ringbuf.h"
#include "uac2_audio_dma.h"

#define UAC2_AUDIO_DEV_PATH         "/dev/audio/pcm0"
#define UAC2_AUDIO_DEV_ALT          "/dev/pcm0"

#define UAC2_AUDIO_NUM_BUFFERS      16
#define UAC2_AUDIO_BUFFER_SIZE      2048   /* 256 frames @ 192kHz stereo 32-bit = 1.33ms */
#define UAC2_PUMP_STACKSIZE         4096

/* Slot bitwidth: 24-bit PCM in 32-bit container */
#define UAC2_SLOTWIDTH_32           32u
#define UAC2_SLOTWIDTH_16           16u

struct uac2_audio_dma_s
{
  int dev_fd;
  mqd_t mq;
  char mq_name[32];
  pthread_t pump_tid;
  sem_t pump_sem;
  bool is_initialized;
  volatile bool is_playing;
  volatile bool thread_run;

  uint32_t sample_rate;
  uint8_t channels;
  uint8_t bit_depth;
  uint8_t volume_percent;
  bool mute;

  /* APB Buffer Management */
  struct ap_buffer_s apbs[UAC2_AUDIO_NUM_BUFFERS] __attribute__((aligned(4)));
  uint8_t buff_mem[UAC2_AUDIO_NUM_BUFFERS][UAC2_AUDIO_BUFFER_SIZE] __attribute__((aligned(16)));
  struct ap_buffer_s *free_apbs[UAC2_AUDIO_NUM_BUFFERS];
  int free_top;

  /* Statistics */
  volatile uint32_t enqueue_count;
  volatile uint32_t dequeue_count;
  volatile uint32_t underrun_count;
  volatile uint32_t restart_needed;
};

static struct uac2_audio_dma_s g_audio_dma;
static Uac2RingBuffer g_pcm_ring;

static void free_pool_push(struct ap_buffer_s *apb)
{
  if (g_audio_dma.free_top >= UAC2_AUDIO_NUM_BUFFERS)
    {
      return;
    }
  for (int i = 0; i < g_audio_dma.free_top; i++)
    {
      if (g_audio_dma.free_apbs[i] == apb)
        {
          return; /* Avoid duplicate */
        }
    }
  g_audio_dma.free_apbs[g_audio_dma.free_top++] = apb;
}

static struct ap_buffer_s *free_pool_pop(void)
{
  if (g_audio_dma.free_top <= 0)
    {
      return NULL;
    }
  return g_audio_dma.free_apbs[--g_audio_dma.free_top];
}

static void drain_audio_msgs(mqd_t mq)
{
  struct audio_msg_s msg;
  for (;;)
    {
      ssize_t size = mq_receive(mq, (char *)&msg, sizeof(msg), NULL);
      if (size != sizeof(msg))
        {
          break; /* EAGAIN: empty queue */
        }
      switch (msg.msg_id)
        {
          case AUDIO_MSG_DEQUEUE:
            g_audio_dma.dequeue_count++;
            if (msg.u.ptr)
              {
                free_pool_push((struct ap_buffer_s *)msg.u.ptr);
              }
            break;
          case AUDIO_MSG_UNDERRUN:
            g_audio_dma.underrun_count++;
            g_audio_dma.restart_needed = 1;
            break;
          case AUDIO_MSG_IOERROR:
            g_audio_dma.restart_needed = 2;
            break;
          default:
            break;
        }
    }
}

static void *uac2_audio_pump_thread(void *arg)
{
  (void)arg;
  uint8_t chunk[UAC2_AUDIO_BUFFER_SIZE];

  printf("[UAC2-AUDIO] Pump thread started (priority 150)\n");

  while (g_audio_dma.thread_run)
    {
      sem_wait(&g_audio_dma.pump_sem);

      if (!g_audio_dma.is_playing || g_audio_dma.dev_fd < 0)
        {
          continue;
        }

      drain_audio_msgs(g_audio_dma.mq);

      /* Handle underrun recovery if needed */
      if (g_audio_dma.restart_needed)
        {
          g_audio_dma.restart_needed = 0;
          drain_audio_msgs(g_audio_dma.mq);

          /* Refill with silence to restart DMA smoothly */
          int refill = 0;
          for (int i = 0; i < 2 && g_audio_dma.free_top > 0; i++)
            {
              struct ap_buffer_s *apb = free_pool_pop();
              if (!apb) break;
              memset(apb->samp, 0, apb->nmaxbytes);
              apb->nbytes = apb->nmaxbytes;
              apb->curbyte = 0;
              struct audio_buf_desc_s bd;
              bd.numbytes = apb->nmaxbytes;
              bd.u.buffer = apb;
              if (ioctl(g_audio_dma.dev_fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)(uintptr_t)&bd) == 0)
                {
                  g_audio_dma.enqueue_count++;
                  refill++;
                }
              else
                {
                  free_pool_push(apb);
                  break;
                }
            }
          if (refill > 0)
            {
              ioctl(g_audio_dma.dev_fd, AUDIOIOC_START, 0);
            }
        }

      /* Feed audio from ring buffer to free APBs */
      while (g_audio_dma.is_playing)
        {
          drain_audio_msgs(g_audio_dma.mq);

          uint32_t avail = uac2_ringbuf_available_read(&g_pcm_ring);
          if (avail < UAC2_AUDIO_BUFFER_SIZE)
            {
              break; /* Wait for more USB packets */
            }

          if (g_audio_dma.free_top <= 0)
            {
              break; /* All DMA buffers in flight */
            }

          struct ap_buffer_s *apb = free_pool_pop();
          if (!apb)
            {
              break;
            }

          uac2_ringbuf_read(&g_pcm_ring, chunk, UAC2_AUDIO_BUFFER_SIZE);
          memcpy(apb->samp, chunk, UAC2_AUDIO_BUFFER_SIZE);
          apb->nbytes = UAC2_AUDIO_BUFFER_SIZE;
          apb->curbyte = 0;

          struct audio_buf_desc_s bd;
          bd.numbytes = UAC2_AUDIO_BUFFER_SIZE;
          bd.u.buffer = apb;

          int ret = ioctl(g_audio_dma.dev_fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)(uintptr_t)&bd);
          if (ret == 0)
            {
              g_audio_dma.enqueue_count++;
            }
          else
            {
              free_pool_push(apb);
              break;
            }
        }
    }

  printf("[UAC2-AUDIO] Pump thread exiting\n");
  return NULL;
}

int uac2_audio_init(uint32_t sample_rate, uint8_t bit_depth, uint8_t channels)
{
  memset(&g_audio_dma, 0, sizeof(g_audio_dma));
  g_audio_dma.dev_fd = -1;
  g_audio_dma.sample_rate = sample_rate;
  g_audio_dma.channels = channels;
  g_audio_dma.bit_depth = bit_depth;
  g_audio_dma.volume_percent = UAC2_AUDIO_DEFAULT_VOLUME;
  g_audio_dma.mute = false;

  uac2_ringbuf_init(&g_pcm_ring);

  printf("[UAC2-AUDIO] Powering on CXD5247 codec...\n");
  board_audio_power_control(true);

  printf("[UAC2-AUDIO] Disabling MIC input circuits (clean DAC mode)...\n");
  cxd56_audio_dis_input();

  printf("[UAC2-AUDIO] Unmuting external headphone amplifier...\n");
  board_external_amp_mute_control(false);

  /* Open audio device */
  g_audio_dma.dev_fd = open(UAC2_AUDIO_DEV_PATH, O_RDWR | O_CLOEXEC);
  if (g_audio_dma.dev_fd < 0)
    {
      g_audio_dma.dev_fd = open(UAC2_AUDIO_DEV_ALT, O_RDWR | O_CLOEXEC);
    }

  if (g_audio_dma.dev_fd < 0)
    {
      printf("[UAC2-AUDIO] ERROR: Failed to open %s (errno=%d)\n",
             UAC2_AUDIO_DEV_PATH, errno);
      return -ENODEV;
    }

  /* Create non-blocking POSIX message queue for DMA callbacks */
  snprintf(g_audio_dma.mq_name, sizeof(g_audio_dma.mq_name), "/mq_uac2_%d", (int)getpid());
  struct mq_attr attr;
  attr.mq_maxmsg  = 32;
  attr.mq_msgsize = sizeof(struct audio_msg_s);
  attr.mq_curmsgs = 0;
  attr.mq_flags   = O_NONBLOCK;

  g_audio_dma.mq = mq_open(g_audio_dma.mq_name, O_RDWR | O_CREAT | O_NONBLOCK, 0644, &attr);
  if (g_audio_dma.mq == (mqd_t)-1)
    {
      printf("[UAC2-AUDIO] ERROR: mq_open failed (errno=%d)\n", errno);
      close(g_audio_dma.dev_fd);
      g_audio_dma.dev_fd = -1;
      return -EIO;
    }

  if (ioctl(g_audio_dma.dev_fd, AUDIOIOC_REGISTERMQ, (unsigned long)g_audio_dma.mq) < 0)
    {
      printf("[UAC2-AUDIO] ERROR: AUDIOIOC_REGISTERMQ failed (errno=%d)\n", errno);
      mq_close(g_audio_dma.mq);
      close(g_audio_dma.dev_fd);
      g_audio_dma.dev_fd = -1;
      return -EIO;
    }

  /* Configure audio format: 192kHz, 2ch, 32-bit container */
  struct audio_caps_desc_s cap_desc;
  memset(&cap_desc, 0, sizeof(cap_desc));
  cap_desc.caps.ac_len = sizeof(struct audio_caps_s);
  cap_desc.caps.ac_type = AUDIO_TYPE_OUTPUT;
  cap_desc.caps.ac_channels = channels;
  cap_desc.caps.ac_controls.hw[0] = (uint16_t)(sample_rate & 0xffff);
  cap_desc.caps.ac_controls.b[2] = UAC2_SLOTWIDTH_32; /* 32-bit slot */
  cap_desc.caps.ac_controls.b[3] = (uint8_t)((sample_rate >> 16) & 0xff);

  int ret = ioctl(g_audio_dma.dev_fd, AUDIOIOC_CONFIGURE, (unsigned long)(uintptr_t)&cap_desc);
  if (ret < 0)
    {
      printf("[UAC2-AUDIO] WARNING: AUDIOIOC_CONFIGURE returned %d\n", ret);
    }

  /* Set initial volume (100% / 0dB) */
  struct audio_caps_desc_s vol_desc;
  memset(&vol_desc, 0, sizeof(vol_desc));
  vol_desc.caps.ac_len = sizeof(struct audio_caps_s);
  vol_desc.caps.ac_type = AUDIO_TYPE_FEATURE;
  vol_desc.caps.ac_format.hw = AUDIO_FU_VOLUME;
  vol_desc.caps.ac_controls.hw[0] = 1000; /* 0dB (max) */
  ioctl(g_audio_dma.dev_fd, AUDIOIOC_CONFIGURE, (unsigned long)(uintptr_t)&vol_desc);

  /* Initialize APB pool */
  g_audio_dma.free_top = 0;
  for (int i = 0; i < UAC2_AUDIO_NUM_BUFFERS; i++)
    {
      g_audio_dma.apbs[i].nmaxbytes = UAC2_AUDIO_BUFFER_SIZE;
      g_audio_dma.apbs[i].nbytes = UAC2_AUDIO_BUFFER_SIZE;
      g_audio_dma.apbs[i].curbyte = 0;
      g_audio_dma.apbs[i].flags = 0;
      g_audio_dma.apbs[i].samp = &g_audio_dma.buff_mem[i][0];
      memset(g_audio_dma.apbs[i].samp, 0, UAC2_AUDIO_BUFFER_SIZE);
      nxmutex_init(&g_audio_dma.apbs[i].lock);

      /* Pre-enqueue silence buffers into DMA queue */
      struct audio_buf_desc_s buf_desc;
      buf_desc.numbytes = UAC2_AUDIO_BUFFER_SIZE;
      buf_desc.u.buffer = &g_audio_dma.apbs[i];
      int enq_ret = ioctl(g_audio_dma.dev_fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)(uintptr_t)&buf_desc);
      if (enq_ret < 0)
        {
          free_pool_push(&g_audio_dma.apbs[i]);
        }
      else
        {
          g_audio_dma.enqueue_count++;
        }
    }

  sem_init(&g_audio_dma.pump_sem, 0, 0);
  g_audio_dma.thread_run = true;

  /* Spawn pump thread with high priority */
  pthread_attr_t pattr;
  pthread_attr_init(&pattr);
  pthread_attr_setstacksize(&pattr, UAC2_PUMP_STACKSIZE);
  struct sched_param sparam;
  sparam.sched_priority = 150;
  pthread_attr_setschedparam(&pattr, &sparam);

  ret = pthread_create(&g_audio_dma.pump_tid, &pattr, uac2_audio_pump_thread, NULL);
  pthread_attr_destroy(&pattr);
  if (ret != 0)
    {
      printf("[UAC2-AUDIO] ERROR: Failed to create pump thread: %d\n", ret);
    }

  g_audio_dma.is_initialized = true;
  printf("[UAC2-AUDIO] CXD5247 S-Master Ready: %lu Hz / %u ch / 32-bit slot\n",
         (unsigned long)sample_rate, channels);
  return 0;
}

int uac2_audio_start(void)
{
  if (!g_audio_dma.is_initialized || g_audio_dma.is_playing)
    {
      return 0;
    }

  g_audio_dma.is_playing = true;
  if (g_audio_dma.dev_fd >= 0)
    {
      ioctl(g_audio_dma.dev_fd, AUDIOIOC_START, 0);
    }
  sem_post(&g_audio_dma.pump_sem);
  printf("[UAC2-AUDIO] CXD5247 DMA Playback started!\n");
  return 0;
}

int uac2_audio_stop(void)
{
  if (!g_audio_dma.is_playing)
    {
      return 0;
    }

  g_audio_dma.is_playing = false;
  if (g_audio_dma.dev_fd >= 0)
    {
      ioctl(g_audio_dma.dev_fd, AUDIOIOC_STOP, 0);
    }
  uac2_ringbuf_init(&g_pcm_ring);
  printf("[UAC2-AUDIO] CXD5247 DMA Playback stopped.\n");
  return 0;
}

int uac2_audio_write(const void *buffer, size_t bytes)
{
  if (buffer == NULL || bytes == 0)
    {
      return 0;
    }

  /* Non-blocking ISR-safe write into lock-free ring buffer */
  uint32_t accepted = uac2_ringbuf_write(&g_pcm_ring, buffer, (uint32_t)bytes);

  /* Wake up pump thread */
  sem_post(&g_audio_dma.pump_sem);

  return (int)accepted;
}

void uac2_audio_set_volume(uint8_t volume_percent)
{
  if (volume_percent > 100u)
    {
      volume_percent = 100u;
    }
  g_audio_dma.volume_percent = volume_percent;

  if (g_audio_dma.dev_fd >= 0)
    {
      uint16_t gain = (uint16_t)((uint32_t)volume_percent * 10u);
      struct audio_caps_desc_s desc;
      memset(&desc, 0, sizeof(desc));
      desc.caps.ac_len = sizeof(struct audio_caps_s);
      desc.caps.ac_type = AUDIO_TYPE_FEATURE;
      desc.caps.ac_format.hw = AUDIO_FU_VOLUME;
      desc.caps.ac_controls.hw[0] = gain;
      ioctl(g_audio_dma.dev_fd, AUDIOIOC_CONFIGURE, (unsigned long)(uintptr_t)&desc);
    }
  printf("[UAC2-AUDIO] Volume: %u%%\n", (unsigned)volume_percent);
}

void uac2_audio_set_mute(bool mute)
{
  g_audio_dma.mute = mute;
  if (g_audio_dma.dev_fd >= 0)
    {
      struct audio_caps_desc_s desc;
      memset(&desc, 0, sizeof(desc));
      desc.caps.ac_len = sizeof(struct audio_caps_s);
      desc.caps.ac_type = AUDIO_TYPE_FEATURE;
      desc.caps.ac_format.hw = AUDIO_FU_MUTE;
      desc.caps.ac_controls.hw[0] = mute ? 1u : 0u;
      ioctl(g_audio_dma.dev_fd, AUDIOIOC_CONFIGURE, (unsigned long)(uintptr_t)&desc);
    }
  printf("[UAC2-AUDIO] Mute: %s\n", mute ? "MUTED" : "UNMUTED");
}

/* Legacy entry points */
int uac2_audio_dma_init(void)
{
  return uac2_audio_init(UAC2_AUDIO_DEFAULT_RATE,
                         UAC2_AUDIO_DEFAULT_DEPTH,
                         UAC2_AUDIO_DEFAULT_CHANNELS);
}

void uac2_audio_dma_start(void)
{
  uac2_audio_start();
}

void uac2_audio_dma_stop(void)
{
  uac2_audio_stop();
}
