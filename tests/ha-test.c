/*
 * Copyright 2013 10gen Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <mongoc.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#ifdef __linux
#include <sys/prctl.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ha-test.h"


mongoc_client_t *
ha_replica_set_create_client (ha_replica_set_t *replica_set)
{
   mongoc_client_t *client;
   bson_string_t *str;
   ha_node_t *iter;
   char *portstr;

   str = bson_string_new("mongodb://");

   for (iter = replica_set->nodes; iter; iter = iter->next) {
      bson_string_append(str, "127.0.0.1:");

      portstr = bson_strdup_printf("%hu", iter->port);
      bson_string_append(str, portstr);
      bson_free(portstr);

      if (iter->next) {
         bson_string_append(str, ",");
      }
   }

   bson_string_append(str, "/?replicaSet=");
   bson_string_append(str, replica_set->name);

   client = mongoc_client_new(str->str);

#ifdef MONGOC_ENABLE_SSL
   if (replica_set->ssl_opt) {
      mongoc_client_set_ssl_opts(client, replica_set->ssl_opt);
   }
#endif

   bson_string_free(str, TRUE);

   return client;
}


static ha_node_t *
ha_node_new (const char       *name,
             const char       *repl_set,
             const char       *dbpath,
             bson_bool_t       is_arbiter,
             bson_bool_t       is_config,
             bson_bool_t       is_router,
             bson_uint16_t     port)
{
   ha_node_t *node;

   node = bson_malloc0(sizeof *node);
   node->name = bson_strdup(name);
   node->repl_set = bson_strdup(repl_set);
   node->dbpath = bson_strdup(dbpath);
   node->is_arbiter = is_arbiter;
   node->is_config = is_config;
   node->is_router = is_router;
   node->port = port;

   return node;
}


void
ha_node_setup (ha_node_t *node)
{
   if (!!mkdir(node->dbpath, 0750)) {
      MONGOC_WARNING("Failed to create directory \"%s\"",
                     node->dbpath);
      abort();
   }
}


void
ha_node_kill (ha_node_t *node)
{
   if (node->pid) {
      int status;

      kill(node->pid, SIGKILL);
      waitpid(node->pid, &status, 0);
      node->pid = 0;
   }
}


void
ha_node_restart (ha_node_t *node)
{
   struct stat st;
   pid_t pid;
   char portstr[12];
   char *argv[30];
   int i = 0;

   snprintf(portstr, sizeof portstr, "%hu", node->port);
   portstr[sizeof portstr - 1] = '\0';

   ha_node_kill(node);

   if (!node->is_router && !node->is_config) {
      argv[i++] = (char *) "mongod";
      argv[i++] = (char *) "--dbpath";
      argv[i++] = (char *) ".";
      argv[i++] = (char *) "--port";
      argv[i++] = portstr;
      argv[i++] = (char *) "--nojournal";
      argv[i++] = (char *) "--noprealloc";
      argv[i++] = (char *) "--smallfiles";
      argv[i++] = (char *) "--nohttpinterface";
      argv[i++] = (char *) "--bind_ip";
      argv[i++] = (char *) "127.0.0.1";
      argv[i++] = (char *) "--replSet";
      argv[i++] = node->repl_set;

#ifdef MONGOC_ENABLE_SSL
      if (node->ssl_opt) {
         if (node->ssl_opt->pem_file) {
            argv[i++] = (char *) "--sslPEMKeyFile";
            argv[i++] = (char *)(node->ssl_opt->pem_file);
            argv[i++] = (char *) "--sslClusterFile";
            argv[i++] = (char *)(node->ssl_opt->pem_file);
         }
         if (node->ssl_opt->ca_file) {
            argv[i++] = (char *) "--sslCAFile";
            argv[i++] = (char *)(node->ssl_opt->ca_file);
         }
         argv[i++] = (char *) "--sslOnNormalPorts";
      }
#endif

      argv[i++] = "--logpath";
      argv[i++] = "log";
      argv[i++] = NULL;
   } else if (node->is_config) {
      argv[i++] = (char *) "mongod";
      argv[i++] = (char *) "--configsvr";
      argv[i++] = (char *) "--dbpath";
      argv[i++] = (char *) ".";
      argv[i++] = (char *) "--port";
      argv[i++] = (char *) portstr;
      argv[i++] = NULL;
   } else {
      argv[i++] = (char *) "mongos";
      argv[i++] = (char *) "--bind_ip";
      argv[i++] = (char *) "127.0.0.1";
      argv[i++] = (char *) "--nohttpinterface";
      argv[i++] = (char *) "--port";
      argv[i++] = (char *) portstr;
      argv[i++] = (char *) "--configdb";
      argv[i++] = node->configopt;
      argv[i++] = NULL;
   }

   pid = fork();
   if (pid < 0) {
      perror("Failed to fork process");
      abort();
   }

   if (!pid) {
      int fd;

#ifdef __linux
      prctl (PR_SET_PDEATHSIG, 15);
#endif

      if (0 != chdir(node->dbpath)) {
         perror("Failed to chdir");
         abort();
      }

      if (0 == stat("mongod.lock", &st)) {
         unlink("mongod.lock");
      }

      fd = open("/dev/null", O_RDWR);
      if (fd == -1) {
         perror("Failed to open /dev/null");
         abort();
      }

      dup2(fd, STDIN_FILENO);
      dup2(fd, STDOUT_FILENO);
      dup2(fd, STDERR_FILENO);

      close(fd);

      if (-1 == execvp(argv[0], argv)) {
         perror("Failed to spawn process");
         abort();
      }
   }

   fprintf(stderr, "[%d]: ", (int)pid);
   for (i = 0; argv[i]; i++)
      fprintf(stderr, "%s ", argv[i]);
   fprintf(stderr, "\n");

   node->pid = pid;
}


static void
ha_node_destroy (ha_node_t *node)
{
   ha_node_kill(node);
   bson_free(node->name);
   bson_free(node->repl_set);
   bson_free(node->dbpath);
   bson_free(node->configopt);
   bson_free(node);
}


static void
random_init (void)
{
   int seed;
   int fd;

   fd = open("/dev/urandom", O_RDONLY);
   if (fd == -1) {
      fprintf(stderr, "Failed to open /dev/urandom\n");
      exit(1);
   }

   if (4 != read(fd, &seed, 4)) {
      fprintf(stderr, "Failed to read from /dev/urandom\n");
      exit(2);
   }

   fprintf(stderr, "srand(%d)\n", seed);
   srand(seed);
}


static int
random_int (void)
{
   static pthread_once_t once = PTHREAD_ONCE_INIT;
   pthread_once(&once, random_init);
   return rand();
}


static int
random_int_range (int low,
                  int high)
{
   return low + (random_int() % (high - low));
}


#ifdef MONGOC_ENABLE_SSL
void
ha_replica_set_ssl (ha_replica_set_t *repl_set,
                    mongoc_ssl_opt_t *opt)
{
   repl_set->ssl_opt = opt;
}
#endif


ha_replica_set_t *
ha_replica_set_new (const char *name)
{
   ha_replica_set_t *repl_set;

   repl_set = bson_malloc0(sizeof *repl_set);
   repl_set->name = bson_strdup(name);
   repl_set->next_port = random_int_range(30000, 40000);

   return repl_set;
}


static ha_node_t *
ha_replica_set_add_node (ha_replica_set_t *replica_set,
                         const char       *name,
                         bson_bool_t       is_arbiter)
{
   ha_node_t *node;
   ha_node_t *iter;
   char dbpath[PATH_MAX];

   snprintf(dbpath, sizeof dbpath, "%s/%s", replica_set->name, name);
   dbpath[sizeof dbpath - 1] = '\0';

   node = ha_node_new(name,
                      replica_set->name,
                      dbpath,
                      is_arbiter,
                      FALSE,
                      FALSE,
                      replica_set->next_port++);
#ifdef MONGOC_ENABLE_SSL
   node->ssl_opt = replica_set->ssl_opt;
#endif

   if (!replica_set->nodes) {
      replica_set->nodes = node;
   } else {
      for (iter = replica_set->nodes; iter->next; iter = iter->next) { }
      iter->next = node;
   }

   return node;
}


ha_node_t *
ha_replica_set_add_arbiter (ha_replica_set_t *replica_set,
                            const char       *name)
{
   return ha_replica_set_add_node(replica_set, name, TRUE);
}


ha_node_t *
ha_replica_set_add_replica (ha_replica_set_t *replica_set,
                            const char       *name)
{
   return ha_replica_set_add_node(replica_set, name, FALSE);
}


static void
ha_replica_set_configure (ha_replica_set_t *replica_set,
                          ha_node_t        *primary)
{
   mongoc_database_t *database;
   mongoc_client_t *client;
   mongoc_cursor_t *cursor;
   const bson_t *doc;
   bson_error_t error;
   bson_iter_t iter;
   ha_node_t *node;
   bson_t ar;
   bson_t cmd;
   bson_t config;
   bson_t member;
   char *str;
   char *uristr;
   char hoststr[32];
   char key[8];
   int i = 0;

   uristr = bson_strdup_printf("mongodb://127.0.0.1:%hu/", primary->port);
   client = mongoc_client_new(uristr);
#ifdef MONGOC_ENABLE_SSL
   if (replica_set->ssl_opt) {
      mongoc_client_set_ssl_opts(client, replica_set->ssl_opt);
   }
#endif
   bson_free(uristr);

   bson_init(&cmd);
   bson_append_document_begin(&cmd, "replSetInitiate", -1, &config);
   bson_append_utf8(&config, "_id", 3, replica_set->name, -1);
   bson_append_array_begin(&config, "members", -1, &ar);
   for (node = replica_set->nodes; node; node = node->next) {
      snprintf(key, sizeof key, "%u", i);
      key[sizeof key - 1] = '\0';

      snprintf(hoststr, sizeof hoststr, "127.0.0.1:%hu", node->port);
      hoststr[sizeof hoststr - 1] = '\0';

      bson_append_document_begin(&ar, key, -1, &member);
      bson_append_int32(&member, "_id", -1, i);
      bson_append_utf8(&member, "host", -1, hoststr, -1);
      bson_append_bool(&member, "arbiterOnly", -1, node->is_arbiter);
      bson_append_document_end(&ar, &member);

      i++;
   }
   bson_append_array_end(&config, &ar);
   bson_append_document_end(&cmd, &config);

   str = bson_as_json(&cmd, NULL);
   MONGOC_DEBUG("Config: %s", str);
   bson_free(str);

   database = mongoc_client_get_database(client, "admin");

again:
   cursor = mongoc_database_command(database,
                                    MONGOC_QUERY_NONE,
                                    0,
                                    1,
                                    0,
                                    &cmd,
                                    NULL,
                                    NULL);

   while (mongoc_cursor_next(cursor, &doc)) {
      str = bson_as_json(doc, NULL);
      MONGOC_DEBUG("Reply: %s", str);
      bson_free(str);
      if (bson_iter_init_find(&iter, doc, "ok") &&
          bson_iter_as_bool(&iter)) {
         goto cleanup;
      }
   }

   if (mongoc_cursor_error(cursor, &error)) {
      mongoc_cursor_destroy(cursor);
      MONGOC_WARNING("%s: Retrying in 1 second.", error.message);
      sleep(1);
      goto again;
   }

cleanup:
   mongoc_cursor_destroy(cursor);
   mongoc_database_destroy(database);
   mongoc_client_destroy(client);
   bson_destroy(&cmd);
}


void
ha_replica_set_start (ha_replica_set_t *replica_set)
{
   struct stat st;
   ha_node_t *primary = NULL;
   ha_node_t *node;
   char *cmd;

   if (!stat(replica_set->name, &st)) {
      if (S_ISDIR(st.st_mode)) {
         /* Ayyyeeeeeee */
         cmd = bson_strdup_printf("rm -rf \"%s\"", replica_set->name);
         fprintf(stderr, "%s\n", cmd);
         system(cmd);
         bson_free(cmd);
      }
   }

   if (!!mkdir(replica_set->name, 0750)) {
      fprintf(stderr, "Failed to create directory \"%s\"\n",
              replica_set->name);
      abort();
   }

   for (node = replica_set->nodes; node; node = node->next) {
      if (!primary && !node->is_arbiter) {
         primary = node;
      }
      ha_node_setup(node);
      ha_node_restart(node);
   }

   BSON_ASSERT(primary);

   sleep(2);

   ha_replica_set_configure(replica_set, primary);
}


void
ha_replica_set_shutdown (ha_replica_set_t *replica_set)
{
   ha_node_t *node;

   for (node = replica_set->nodes; node; node = node->next) {
      ha_node_kill(node);
   }
}


void
ha_replica_set_destroy (ha_replica_set_t *replica_set)
{
   ha_node_t *node;

   while ((node = replica_set->nodes)) {
      replica_set->nodes = node->next;
      ha_node_destroy(node);
   }

   bson_free(replica_set->name);
   bson_free(replica_set);
}


static bson_bool_t
ha_replica_set_get_status (ha_replica_set_t *replica_set,
                           bson_t           *status)
{
   mongoc_database_t *db;
   mongoc_client_t *client;
   mongoc_cursor_t *cursor;
   const bson_t *doc;
   bson_bool_t ret = FALSE;
   ha_node_t *node;
   bson_t cmd;
   char *uristr;

   bson_init(&cmd);
   bson_append_int32(&cmd, "replSetGetStatus", -1, 1);

   for (node = replica_set->nodes; !ret && node; node = node->next) {
      uristr = bson_strdup_printf("mongodb://127.0.0.1:%hu/?slaveOk=true",
                                  node->port);
      client = mongoc_client_new(uristr);
#ifdef MONGOC_ENABLE_SSL
      if (replica_set->ssl_opt) {
         mongoc_client_set_ssl_opts(client, replica_set->ssl_opt);
      }
#endif
      bson_free(uristr);

      db = mongoc_client_get_database(client, "admin");

      if ((cursor = mongoc_database_command(db,
                                            MONGOC_QUERY_SLAVE_OK,
                                            0,
                                            1,
                                            0,
                                            &cmd,
                                            NULL,
                                            NULL))) {
         if (mongoc_cursor_next(cursor, &doc)) {
            bson_copy_to(doc, status);
            ret = TRUE;
         }
         mongoc_cursor_destroy(cursor);
      }

      mongoc_database_destroy(db);
      mongoc_client_destroy(client);
   }

   return ret;
}


void
ha_replica_set_wait_for_healthy (ha_replica_set_t *replica_set)
{
   bson_iter_t iter;
   bson_iter_t ar;
   bson_iter_t member;
   const char *stateStr;
   bson_t status;

again:
   sleep(1);

   if (!ha_replica_set_get_status(replica_set, &status)) {
      MONGOC_INFO("Failed to get replicaSet status. "
                  "Sleeping 1 second.");
      goto again;
   }

#if 0
   {
      char *str;

      str = bson_as_json(&status, NULL);
      printf("%s\n", str);
      bson_free(str);
   }
#endif

   if (!bson_iter_init_find(&iter, &status, "members") ||
       !BSON_ITER_HOLDS_ARRAY(&iter) ||
       !bson_iter_recurse(&iter, &ar)) {
      bson_destroy(&status);
      MONGOC_INFO("ReplicaSet has not yet come online. "
                  "Sleeping 1 second.");
      goto again;
   }

   while (bson_iter_next(&ar)) {
      if (BSON_ITER_HOLDS_DOCUMENT(&ar) &&
          bson_iter_recurse(&ar, &member) &&
          bson_iter_find(&member, "stateStr") &&
          (stateStr = bson_iter_utf8(&member, NULL))) {
         if (!!strcmp(stateStr, "PRIMARY") &&
             !!strcmp(stateStr, "SECONDARY") &&
             !!strcmp(stateStr, "ARBITER")) {
            bson_destroy(&status);
            MONGOC_INFO("Found unhealthy node. Sleeping 1 second.");
            goto again;
         }
      }
   }

   bson_destroy(&status);
}


ha_sharded_cluster_t *
ha_sharded_cluster_new (const char *name)
{
   ha_sharded_cluster_t *cluster;

   cluster = bson_malloc0(sizeof *cluster);
   cluster->next_port = random_int_range(40000, 41000);
   cluster->name = bson_strdup(name);

   return cluster;
}


void
ha_sharded_cluster_add_replica_set (ha_sharded_cluster_t *cluster,
                                    ha_replica_set_t     *replica_set)
{
   int i;

   bson_return_if_fail(cluster);
   bson_return_if_fail(replica_set);

   for (i = 0; i < 12; i++) {
      if (!cluster->replicas[i]) {
         cluster->replicas[i] = replica_set;
         break;
      }
   }
}


ha_node_t *
ha_sharded_cluster_add_config (ha_sharded_cluster_t *cluster,
                               const char           *name)
{
   ha_node_t *node;
   char dbpath[PATH_MAX];

   bson_return_val_if_fail(cluster, NULL);

   snprintf(dbpath, sizeof dbpath, "%s/%s", cluster->name, name);
   dbpath[sizeof dbpath - 1] = '\0';

   node = ha_node_new(name,
                      NULL,
                      dbpath,
                      FALSE,
                      TRUE,
                      FALSE,
                      cluster->next_port++);
#ifdef MONGOC_ENABLE_SSL
   node->ssl_opt = cluster->ssl_opt;
#endif
   node->next = cluster->configs;
   cluster->configs = node;

   return node;
}


ha_node_t *
ha_sharded_cluster_add_router (ha_sharded_cluster_t *cluster,
                               const char           *name)
{
   ha_node_t *node;
   char dbpath[PATH_MAX];

   bson_return_val_if_fail(cluster, NULL);

   snprintf(dbpath, sizeof dbpath, "%s/%s", cluster->name, name);
   dbpath[sizeof dbpath - 1] = '\0';

   node = ha_node_new(name,
                      NULL,
                      dbpath,
                      FALSE,
                      FALSE,
                      TRUE,
                      cluster->next_port++);
#ifdef MONGOC_ENABLE_SSL
   node->ssl_opt = cluster->ssl_opt;
#endif
   node->next = cluster->routers;
   cluster->routers = node;

   return node;
}


static void
ha_config_add_shard (ha_node_t        *node,
                     ha_replica_set_t *replica_set)
{
   mongoc_collection_t *collection;
   mongoc_client_t *client;
   bson_string_t *shardstr;
   bson_error_t error;
   bson_bool_t r;
   bson_t reply;
   bson_t cmd = BSON_INITIALIZER;
   char *uristr;

   uristr = bson_strdup_printf ("mongodb://127.0.0.1:%hu/", node->port);
   client = mongoc_client_new (uristr);
   collection = mongoc_client_get_collection (client, "admin", "fake");

   shardstr = bson_string_new (NULL);
   bson_string_append_printf (shardstr, "%s/127.0.0.1:%hu",
                              replica_set->name,
                              replica_set->nodes->port);

   bson_append_utf8 (&cmd, "addShard", -1, shardstr->str, shardstr->len);

   bson_string_free (shardstr, TRUE);

again:
   sleep (1);

   r = mongoc_collection_command_simple (collection, &cmd, NULL, &reply, &error);

   if (!r) {
      fprintf (stderr, "%s\n", error.message);
      goto again;
   }

#if 1
   {
      char *str;

      str = bson_as_json (&reply, NULL);
      printf ("%s\n", str);
      bson_free (str);
   }
#endif

   bson_destroy (&reply);
   bson_destroy (&cmd);
   mongoc_collection_destroy (collection);
   mongoc_client_destroy (client);
   bson_free (uristr);
}


void
ha_sharded_cluster_start (ha_sharded_cluster_t *cluster)
{
   bson_string_t *configopt;
   struct stat st;
   ha_node_t *iter;
   char *cmd;
   int i;

   bson_return_if_fail(cluster);

   if (!stat(cluster->name, &st)) {
      if (S_ISDIR(st.st_mode)) {
         /* Ayyyeeeeeee */
         cmd = bson_strdup_printf("rm -rf \"%s\"", cluster->name);
         fprintf(stderr, "%s\n", cmd);
         system(cmd);
         bson_free(cmd);
      }
   }

   if (!!mkdir(cluster->name, 0750)) {
      fprintf(stderr, "Failed to create directory \"%s\"\n",
              cluster->name);
      abort();
   }

   for (i = 0; i < 12; i++) {
      if (cluster->replicas[i]) {
         ha_replica_set_start(cluster->replicas[i]);
      }
   }

   configopt = bson_string_new (NULL);

   for (iter = cluster->configs; iter; iter = iter->next) {
      ha_node_setup(iter);
      ha_node_restart(iter);
      bson_string_append_printf (configopt, "127.0.0.1:%hu%s",
                                 iter->port,
                                 iter->next ? "," : "");
   }

   sleep (5);

   for (iter = cluster->routers; iter; iter = iter->next) {
      bson_free (iter->configopt);
      iter->configopt = bson_strdup (configopt->str);
      ha_node_setup (iter);
      ha_node_restart (iter);
   }

   ha_sharded_cluster_wait_for_healthy (cluster);

   for (i = 0; i < 12; i++) {
      if (cluster->replicas[i]) {
         ha_config_add_shard (cluster->routers, cluster->replicas[i]);
      }
   }

   bson_string_free (configopt, TRUE);
}


void
ha_sharded_cluster_wait_for_healthy (ha_sharded_cluster_t *cluster)
{
   int i;

   bson_return_if_fail(cluster);

   for (i = 0; i < 12; i++) {
      if (cluster->replicas[i]) {
         ha_replica_set_wait_for_healthy(cluster->replicas[i]);
      }
   }
}


void
ha_sharded_cluster_shutdown (ha_sharded_cluster_t *cluster)
{
   ha_node_t *iter;
   int i;

   bson_return_if_fail(cluster);

   for (i = 0; i < 12; i++) {
      if (cluster->replicas[i]) {
         ha_replica_set_shutdown(cluster->replicas[i]);
      }
   }

   for (iter = cluster->configs; iter; iter = iter->next) {
      ha_node_kill(iter);
   }

   for (iter = cluster->routers; iter; iter = iter->next) {
      ha_node_kill(iter);
   }
}


mongoc_client_t *
ha_sharded_cluster_get_client (ha_sharded_cluster_t *cluster)
{
   const ha_node_t *iter;
   mongoc_client_t *client;
   bson_string_t *str;

   BSON_ASSERT (cluster);
   BSON_ASSERT (cluster->routers);

   str = bson_string_new ("mongodb://");

   for (iter = cluster->routers; iter; iter = iter->next) {
      bson_string_append_printf (str, "127.0.0.1:%hu%s",
                                 iter->port,
                                 iter->next ? "," : "");
   }

   bson_string_append (str, "/");

   client = mongoc_client_new (str->str);

   bson_string_free (str, TRUE);

   return client;
}
