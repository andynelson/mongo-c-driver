#include <mongoc.h>
#include <mongoc-client-private.h>
#include <mongoc-cursor-private.h>
#include <mongoc-log.h>

#include "mongoc-tests.h"


#define HOST (getenv("MONGOC_TEST_HOST") ? getenv("MONGOC_TEST_HOST") : "localhost")


static char *gTestUri;


static void
test_insert (void)
{
   mongoc_collection_t *collection;
   mongoc_client_t *client;
   bson_context_t *context;
   bson_error_t error;
   bson_bool_t r;
   bson_oid_t oid;
   unsigned i;
   bson_t b;

   client = mongoc_client_new(gTestUri);
   assert(client);

   collection = mongoc_client_get_collection(client, "test", "test");
   assert(collection);

   mongoc_collection_drop(collection, &error);

   context = bson_context_new(BSON_CONTEXT_NONE);
   assert(context);

   for (i = 0; i < 10; i++) {
      bson_init(&b);
      bson_oid_init(&oid, context);
      bson_append_oid(&b, "_id", 3, &oid);
      bson_append_utf8(&b, "hello", 5, "/world", 5);
      r = mongoc_collection_insert(collection, MONGOC_INSERT_NONE, &b, NULL,
                                   &error);
      if (!r) {
         MONGOC_WARNING("%s\n", error.message);
      }
      assert(r);
      bson_destroy(&b);
   }

   mongoc_collection_destroy(collection);
   bson_context_destroy(context);
   mongoc_client_destroy(client);
}


static void
test_insert_bulk (void)
{
   mongoc_collection_t *collection;
   mongoc_client_t *client;
   bson_context_t *context;
   bson_error_t error;
   bson_bool_t r;
   bson_oid_t oid;
   unsigned i;
   bson_t q;
   bson_t b[10];
   bson_t *bptr[10];
   bson_int64_t count;

   client = mongoc_client_new(gTestUri);
   assert(client);

   collection = mongoc_client_get_collection(client, "test", "test");
   assert(collection);

   mongoc_collection_drop(collection, &error);

   context = bson_context_new(BSON_CONTEXT_NONE);
   assert(context);

   bson_init(&q);
   bson_append_int32(&q, "n", -1, 0);

   for (i = 0; i < 10; i++) {
      bson_init(&b[i]);
      bson_oid_init(&oid, context);
      bson_append_oid(&b[i], "_id", -1, &oid);
      bson_append_int32(&b[i], "n", -1, i % 2);
      bptr[i] = &b[i];
   }

   r = mongoc_collection_insert_bulk (collection, MONGOC_INSERT_NONE,
                                     (const bson_t **)bptr, 10, NULL, &error);

   if (!r) {
      MONGOC_WARNING("%s\n", error.message);
   }
   assert(r);

   count = mongoc_collection_count (collection, MONGOC_QUERY_NONE, &q, 0, 0, NULL, &error);
   assert(count == 5);

   for (i = 8; i < 10; i++) {
      bson_destroy(&b[i]);
      bson_init(&b[i]);
      bson_oid_init(&oid, context);
      bson_append_oid(&b[i], "_id", -1, &oid);
      bson_append_int32(&b[i], "n", -1, i % 2);
      bptr[i] = &b[i];
   }

   r = mongoc_collection_insert_bulk (collection, MONGOC_INSERT_NONE,
                                     (const bson_t **)bptr, 10, NULL, &error);

   assert(!r);
   assert(error.code == 11000);

   count = mongoc_collection_count (collection, MONGOC_QUERY_NONE, &q, 0, 0, NULL, &error);
   assert(count == 5);

   r = mongoc_collection_insert_bulk (collection, MONGOC_INSERT_CONTINUE_ON_ERROR,
                                     (const bson_t **)bptr, 10, NULL, &error);
   assert(!r);
   assert(error.code == 11000);

   count = mongoc_collection_count (collection, MONGOC_QUERY_NONE, &q, 0, 0, NULL, &error);
   assert(count == 6);

   bson_destroy(&q);
   for (i = 0; i < 10; i++) {
      bson_destroy(&b[i]);
   }

   mongoc_collection_destroy(collection);
   bson_context_destroy(context);
   mongoc_client_destroy(client);
}


static void
test_regex (void)
{
   mongoc_collection_t *collection;
   mongoc_client_t *client;
   bson_error_t error = { 0 };
   bson_int64_t count;
   bson_t q = BSON_INITIALIZER;

   client = mongoc_client_new (gTestUri);
   assert (client);

   collection = mongoc_client_get_collection (client, "test", "test");
   assert (collection);

   bson_append_regex (&q, "hello", -1, "^/wo", NULL);

   count = mongoc_collection_count (collection,
                                    MONGOC_QUERY_NONE,
                                    &q,
                                    0,
                                    0,
                                    NULL,
                                    &error);

   assert (count > 0);
   assert (!error.domain);

   bson_destroy (&q);
   mongoc_collection_destroy (collection);
   mongoc_client_destroy (client);
}


static void
test_update (void)
{
   mongoc_collection_t *collection;
   mongoc_client_t *client;
   bson_context_t *context;
   bson_error_t error;
   bson_bool_t r;
   bson_oid_t oid;
   unsigned i;
   bson_t b;
   bson_t q;
   bson_t u;
   bson_t set;

   client = mongoc_client_new(gTestUri);
   assert(client);

   collection = mongoc_client_get_collection(client, "test", "test");
   assert(collection);

   context = bson_context_new(BSON_CONTEXT_NONE);
   assert(context);

   for (i = 0; i < 10; i++) {
      bson_init(&b);
      bson_oid_init(&oid, context);
      bson_append_oid(&b, "_id", 3, &oid);
      bson_append_utf8(&b, "utf8", 4, "utf8 string", 11);
      bson_append_int32(&b, "int32", 5, 1234);
      bson_append_int64(&b, "int64", 5, 12345678);
      bson_append_bool(&b, "bool", 4, 1);

      r = mongoc_collection_insert(collection, MONGOC_INSERT_NONE, &b, NULL, &error);
      if (!r) {
         MONGOC_WARNING("%s\n", error.message);
      }
      assert(r);

      bson_init(&q);
      bson_append_oid(&q, "_id", 3, &oid);

      bson_init(&u);
      bson_append_document_begin(&u, "$set", 4, &set);
      bson_append_utf8(&set, "utf8", 4, "updated", 7);
      bson_append_document_end(&u, &set);

      r = mongoc_collection_update(collection, MONGOC_UPDATE_NONE, &q, &u, NULL, &error);
      if (!r) {
         MONGOC_WARNING("%s\n", error.message);
      }
      assert(r);

      bson_destroy(&b);
      bson_destroy(&q);
      bson_destroy(&u);
   }

   mongoc_collection_destroy(collection);
   bson_context_destroy(context);
   mongoc_client_destroy(client);
}


static void
test_delete (void)
{
   mongoc_collection_t *collection;
   mongoc_client_t *client;
   bson_context_t *context;
   bson_error_t error;
   bson_bool_t r;
   bson_oid_t oid;
   bson_t b;
   int i;

   client = mongoc_client_new(gTestUri);
   assert(client);

   collection = mongoc_client_get_collection(client, "test", "test");
   assert(collection);

   context = bson_context_new(BSON_CONTEXT_NONE);
   assert(context);

   for (i = 0; i < 100; i++) {
      bson_init(&b);
      bson_oid_init(&oid, context);
      bson_append_oid(&b, "_id", 3, &oid);
      bson_append_utf8(&b, "hello", 5, "world", 5);
      r = mongoc_collection_insert(collection, MONGOC_INSERT_NONE, &b, NULL,
                                   &error);
      if (!r) {
         MONGOC_WARNING("%s\n", error.message);
      }
      assert(r);
      bson_destroy(&b);

      bson_init(&b);
      bson_append_oid(&b, "_id", 3, &oid);
      r = mongoc_collection_delete(collection, MONGOC_DELETE_NONE, &b, NULL,
                                   &error);
      if (!r) {
         MONGOC_WARNING("%s\n", error.message);
      }
      assert(r);
      bson_destroy(&b);
   }

   mongoc_collection_destroy(collection);
   bson_context_destroy(context);
   mongoc_client_destroy(client);
}

static void
test_index (void)
{
   mongoc_collection_t *collection;
   mongoc_client_t *client;
   mongoc_index_opt_t opt;
   bson_error_t error;
   bson_bool_t r;
   bson_t keys;

   mongoc_index_opt_init(&opt);

   client = mongoc_client_new(gTestUri);
   assert(client);

   collection = mongoc_client_get_collection(client, "test", "test");
   assert(collection);

   bson_init(&keys);
   bson_append_int32(&keys, "hello", -1, 1);
   r = mongoc_collection_ensure_index(collection, &keys, &opt, &error);
   assert(r);

   r = mongoc_collection_ensure_index(collection, &keys, &opt, &error);
   assert(r);

   r = mongoc_collection_drop_index(collection, "hello_1", &error);
   assert(r);

   bson_destroy(&keys);

   mongoc_collection_destroy(collection);
   mongoc_client_destroy(client);
}


static void
test_count (void)
{
   mongoc_collection_t *collection;
   mongoc_client_t *client;
   bson_error_t error;
   bson_int64_t count;
   bson_t b;

   client = mongoc_client_new(gTestUri);
   assert(client);

   collection = mongoc_client_get_collection(client, "test", "test");
   assert(collection);

   bson_init(&b);
   count = mongoc_collection_count(collection, MONGOC_QUERY_NONE, &b,
                                   0, 0, NULL, &error);
   bson_destroy(&b);

   if (count == -1) {
      MONGOC_WARNING("%s\n", error.message);
   }
   assert(count != -1);

   mongoc_collection_destroy(collection);
   mongoc_client_destroy(client);
}


static void
test_drop (void)
{
   mongoc_collection_t *collection;
   mongoc_client_t *client;
   bson_error_t error;
   bson_bool_t r;

   client = mongoc_client_new(gTestUri);
   assert(client);

   collection = mongoc_client_get_collection(client, "test", "test");
   assert(collection);

   r = mongoc_collection_drop(collection, &error);
   assert(r == TRUE);

   r = mongoc_collection_drop(collection, &error);
   assert(r == FALSE);

   mongoc_collection_destroy(collection);
   mongoc_client_destroy(client);
}


static void
test_aggregate (void)
{
   mongoc_collection_t *collection;
   mongoc_client_t *client;
   mongoc_cursor_t *cursor;
   const bson_t *doc;
   bson_error_t error;
   bson_bool_t r;
   bson_t b;
   bson_t match;
   bson_t pipeline;
   bson_iter_t iter;

   bson_init(&b);
   bson_append_utf8(&b, "hello", -1, "world", -1);

   bson_init(&match);
   bson_append_document(&match, "$match", -1, &b);

   bson_init(&pipeline);
   bson_append_document(&pipeline, "0", -1, &match);

   client = mongoc_client_new(gTestUri);
   assert(client);

   collection = mongoc_client_get_collection(client, "test", "test");
   assert(collection);

   mongoc_collection_drop(collection, &error);

   r = mongoc_collection_insert(collection, MONGOC_INSERT_NONE, &b, NULL, &error);
   assert(r);

   cursor = mongoc_collection_aggregate(collection, MONGOC_QUERY_NONE, &pipeline, NULL);
   assert(cursor);

   /*
    * This can fail if we are connecting to a pre-2.5.x MongoDB instance.
    */
   r = mongoc_cursor_next(cursor, &doc);
   if (mongoc_cursor_error(cursor, &error)) {
      MONGOC_WARNING("%s", error.message);
   }

   assert(r);
   assert(doc);

   assert (bson_iter_init_find (&iter, doc, "hello") &&
           BSON_ITER_HOLDS_UTF8 (&iter));

   r = mongoc_cursor_next(cursor, &doc);
   if (mongoc_cursor_error(cursor, &error)) {
      MONGOC_WARNING("%s", error.message);
   }
   assert(!r);
   assert(!doc);

   mongoc_cursor_destroy(cursor);
   mongoc_collection_destroy(collection);
   mongoc_client_destroy(client);
   bson_destroy(&b);
   bson_destroy(&pipeline);
   bson_destroy(&match);
}


static void
log_handler (mongoc_log_level_t  log_level,
             const char         *domain,
             const char         *message,
             void               *user_data)
{
   /* Do Nothing */
}


int
main (int   argc,
      char *argv[])
{
   if (argc <= 1 || !!strcmp(argv[1], "-v")) {
      mongoc_log_set_handler(log_handler, NULL);
   }

   gTestUri = bson_strdup_printf("mongodb://%s/", HOST);

   run_test("/mongoc/collection/insert_bulk", test_insert_bulk);
   run_test("/mongoc/collection/insert", test_insert);
   run_test("/mongoc/collection/index", test_index);
   run_test("/mongoc/collection/regex", test_regex);
   run_test("/mongoc/collection/update", test_update);
   run_test("/mongoc/collection/delete", test_delete);
   run_test("/mongoc/collection/count", test_count);
   run_test("/mongoc/collection/drop", test_drop);
   run_test("/mongoc/collection/aggregate", test_aggregate);

   bson_free(gTestUri);

   return 0;
}
