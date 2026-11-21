// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <pthread.h>
#include <stddef.h>

static const size_t kThreadStackBytes = 8u * 1024u * 1024u;

static int CreateWithLinuxStack(pthread_t* thread, const pthread_attr_t* attr,
                                void* (*start)(void*), void* arg) {
  if (attr != NULL)
    return pthread_create(thread, attr, start, arg);
  pthread_attr_t sized;
  pthread_attr_init(&sized);
  pthread_attr_setstacksize(&sized, kThreadStackBytes);
  const int result = pthread_create(thread, &sized, start, arg);
  pthread_attr_destroy(&sized);
  return result;
}

__attribute__((used)) static const struct {
  const void* replacement;
  const void* replacee;
} kInterpose __attribute__((section("__DATA,__interpose"))) = {
    (const void*)CreateWithLinuxStack, (const void*)pthread_create};
