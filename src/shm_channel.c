#include "shm_channel.h"

#include "shm_comm.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>

int shm_create_channel (char name[NAME_LEN], int size, int readers)
{
  if (size < 1)
  {
    return SHM_INVAL;
  }

  if (readers < 1)
  {
    return SHM_INVAL;
  }

  char name_hdr[NAME_LEN + 5];

  strcpy (name_hdr, name);
  strcat (name_hdr, "_hdr");

  int shm_hdr_fd = shm_open (name_hdr, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
  if (shm_hdr_fd < 0)
  {
    return -1;
  }

  if (ftruncate (shm_hdr_fd, CHANNEL_HDR_SIZE(size, readers)) != 0)
  {
    close (shm_hdr_fd);
    shm_unlink (name_hdr);
    return -1;
  }

  void *shm_hdr = mmap (NULL, CHANNEL_HDR_SIZE(size, readers), PROT_READ | PROT_WRITE, MAP_SHARED, shm_hdr_fd, 0);

  if (shm_hdr == MAP_FAILED)
  {
    close (shm_hdr_fd);
    shm_unlink (name_hdr);
    return -1;
  }

  char name_data[NAME_LEN + 5];
  strcpy (name_data, name);
  strcat (name_data, "_data");

  int shm_data_fd = shm_open (name_data, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
  if (shm_data_fd < 0)
  {
    munmap (shm_hdr, CHANNEL_HDR_SIZE(size, readers));
    close (shm_hdr_fd);
    shm_unlink (name_hdr);
    return -1;
  }

  if (ftruncate (shm_data_fd, CHANNEL_DATA_SIZE(size, readers)) != 0)
  {
    munmap (shm_hdr, CHANNEL_HDR_SIZE(size, readers));
    close (shm_hdr_fd);
    shm_unlink (name_hdr);

    close (shm_data_fd);
    shm_unlink (name_data);
    return -1;
  }

  void *shm_data = mmap (NULL, CHANNEL_DATA_SIZE(size, readers), PROT_READ | PROT_WRITE, MAP_SHARED, shm_data_fd, 0);

  if (shm_hdr == MAP_FAILED)
  {
    munmap (shm_hdr, CHANNEL_HDR_SIZE(size, readers));
    close (shm_hdr_fd);
    shm_unlink (name_hdr);

    close (shm_data_fd);
    shm_unlink (name_data);
    return -1;
  }

  memset (shm_data, 0, CHANNEL_DATA_SIZE(size, readers));

  init_channel_hdr (size, readers, SHM_SHARED, shm_hdr);

  munmap (shm_hdr, CHANNEL_HDR_SIZE(size, readers));
  munmap (shm_data, CHANNEL_DATA_SIZE(size, readers));
  close (shm_hdr_fd);
  close (shm_data_fd);

  return 0;
}

int shm_remove_channel (char name[NAME_LEN])
{
  char shm_name_tmp[NAME_LEN + 5];

  strcpy (shm_name_tmp, name);
  strcat (shm_name_tmp, "_data");
  shm_unlink (shm_name_tmp);

  strcpy (shm_name_tmp, name);
  strcat (shm_name_tmp, "_hdr");
  shm_unlink (shm_name_tmp);

  return 0;
}

struct shm_writer
{
  int hdr_fd;
  int data_fd;
  channel_t channel;
  writer_t writer;
};

shm_writer_t *shm_connect_writer (char name[NAME_LEN])
{
  shm_writer_t *ret = malloc (sizeof(shm_writer_t));

  if (ret == NULL)
  {
    return NULL;
  }

  channel_hdr_t *shm_hdr;
  void *shm_data;

  char name_hdr[NAME_LEN + 5];
  strcpy (name_hdr, name);
  strcat (name_hdr, "_hdr");

  char name_data[NAME_LEN + 5];
  strcpy (name_data, name);
  strcat (name_data, "_data");

  ret->hdr_fd = shm_open (name_hdr, O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
  if (ret->hdr_fd < 0)
  {
    free (ret);
    return NULL;
  }

  ret->data_fd = shm_open (name_data, O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
  if (ret->data_fd < 0)
  {
    close (ret->hdr_fd);
    free (ret);
    return NULL;
  }

  struct stat sb;
  fstat (ret->hdr_fd, &sb);

  shm_hdr = mmap (NULL, sb.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, ret->hdr_fd, 0);

  if (shm_hdr == MAP_FAILED)
  {
    close (ret->hdr_fd);
    close (ret->data_fd);
    free (ret);
    return NULL;
  }

  shm_data = mmap (NULL, CHANNEL_DATA_SIZE(shm_hdr->size, shm_hdr->max_readers), PROT_READ | PROT_WRITE, MAP_SHARED,
      ret->data_fd, 0);

  if (shm_data == MAP_FAILED)
  {
    munmap (shm_hdr, sb.st_size);
    close (ret->hdr_fd);
    close (ret->data_fd);
    free (ret);
    return NULL;
  }

  init_channel (shm_hdr, shm_data, &ret->channel);
  create_writer (&ret->channel, &ret->writer);

  return ret;
}

int shm_release_writer (shm_writer_t *writer)
{
  if (writer == NULL)
  {
    return SHM_INVAL;
  }

  release_writer (&writer->writer);

  munmap (writer->channel.buffer, CHANNEL_DATA_SIZE(writer->channel.hdr->size, writer->channel.hdr->max_readers));
  writer->channel.buffer = NULL;
  close (writer->data_fd);
  writer->data_fd = 0;

  munmap (writer->channel.hdr, CHANNEL_HDR_SIZE(writer->channel.hdr->size, writer->channel.hdr->max_readers));
  writer->channel.hdr = NULL;
  writer->channel.reader_ids = NULL;
  writer->channel.reading = NULL;
  close (writer->data_fd);
  writer->data_fd = 0;
  free (writer);

  return 0;
}

int shm_writer_buffer_get (shm_writer_t *wr, void** buf)
{
  if (wr == NULL)
  {
    return SHM_INVAL;
  }

  return writer_buffer_get(&wr->writer, buf);
}

int shm_writer_buffer_write (shm_writer_t *wr)
{
  if (wr == NULL)
  {
    return SHM_INVAL;
  }

  return writer_buffer_write(&wr->writer);
}

int shm_writer_get_size(shm_writer_t *wr)
{
  return wr->channel.hdr->size;
}

struct shm_reader
{
  int hdr_fd;
  int data_fd;
  channel_t channel;
  reader_t reader;
};

shm_reader_t *shm_connect_reader (char name[NAME_LEN])
{
  shm_reader_t *ret = malloc (sizeof(shm_reader_t));

  if (ret == NULL)
  {
    return NULL;
  }

  channel_hdr_t *shm_hdr;
  void *shm_data;

  char name_hdr[NAME_LEN + 5];
  strcpy (name_hdr, name);
  strcat (name_hdr, "_hdr");

  char name_data[NAME_LEN + 5];
  strcpy (name_data, name);
  strcat (name_data, "_data");

  ret->hdr_fd = shm_open (name_hdr, O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
  if (ret->hdr_fd < 0)
  {
    free (ret);
    return NULL;
  }

  ret->data_fd = shm_open (name_data, O_RDONLY, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
  if (ret->data_fd < 0)
  {
    close (ret->hdr_fd);
    free (ret);
    return NULL;
  }

  struct stat sb;
  fstat (ret->hdr_fd, &sb);

  shm_hdr = mmap (NULL, sb.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, ret->hdr_fd, 0);

  if (shm_hdr == MAP_FAILED)
  {
    close (ret->hdr_fd);
    close (ret->data_fd);
    free (ret);
    return NULL;
  }

  shm_data = mmap (NULL, CHANNEL_DATA_SIZE(shm_hdr->size, shm_hdr->max_readers), PROT_READ, MAP_SHARED,
      ret->data_fd, 0);

  if (shm_data == MAP_FAILED)
  {
    munmap (shm_hdr, sb.st_size);
    close (ret->hdr_fd);
    close (ret->data_fd);
    free (ret);
    return NULL;
  }

  init_channel (shm_hdr, shm_data, &ret->channel);
  create_reader (&ret->channel, &ret->reader);

  return ret;
}

int shm_release_reader (shm_reader_t *reader)
{
  if (reader == NULL)
  {
    return SHM_INVAL;
  }

  release_reader (&reader->reader);

  munmap (reader->channel.buffer, CHANNEL_DATA_SIZE(reader->channel.hdr->size, reader->channel.hdr->max_readers));
  reader->channel.buffer = NULL;
  close (reader->data_fd);
  reader->data_fd = 0;

  munmap (reader->channel.hdr, CHANNEL_HDR_SIZE(writer->channel.hdr->size, reader->channel.hdr->max_readers));
  reader->channel.hdr = NULL;
  reader->channel.reader_ids = NULL;
  reader->channel.reading = NULL;
  close (reader->data_fd);
  reader->data_fd = 0;
  free (reader);

  return 0;
}

int shm_reader_buffer_get (shm_reader_t *reader, void **buf)
{
  if (reader == NULL)
  {
    return SHM_INVAL;
  }

  return reader_buffer_get(&reader->reader, buf);
}

int shm_reader_buffer_wait (shm_reader_t *reader, void **buf)
{
  if (reader == NULL)
  {
    return SHM_INVAL;
  }

  return reader_buffer_wait(&reader->reader, buf);
}

int shm_reader_buffer_timedwait (shm_reader_t *reader, const struct timespec *abstime, void **buf)
{
  if (reader == NULL)
  {
    return SHM_INVAL;
  }

  return reader_buffer_timedwait(&reader->reader, abstime, buf);
}