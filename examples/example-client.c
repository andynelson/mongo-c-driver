/* gcc example.c -o example $(pkg-config --cflags --libs libmongoc-1.0) */

#include <mongoc.h>
#include <stdio.h>
#include <stdlib.h>

int
main (int   argc,
      char *argv[])
{
   mongoc_client_t *client;
   mongoc_collection_t *collection;
   mongoc_cursor_t *cursor;
   bson_error_t error;
   const bson_t *doc;
   bson_t query;
   char *str;

   client = mongoc_client_new ("mongodb://localhost:27017/");

   if (!client) {
      fprintf (stderr, "Failed to parse URI.\n");
      return EXIT_FAILURE;
   }

   bson_init (&query);
   bson_append_utf8 (&query, "hello", -1, "world", -1);


   collection = mongoc_client_get_collection (client, "test", "test");
   cursor = mongoc_collection_find (collection,
                                    MONGOC_QUERY_NONE,
                                    0,
                                    0,
                                    0,
                                    &query,
                                    NULL,  /* Fields, NULL for all. */
                                    NULL); /* Read Prefs, NULL for default */

   while (mongoc_cursor_next (cursor, &doc)) {
      str = bson_as_json (doc, NULL);
      fprintf (stdout, "%s\n", str);
      bson_free (str);
   }

   if (mongoc_cursor_error (cursor, &error)) {
      fprintf (stderr, "Cursor Failure: %s\n", error.message);
   }

   bson_destroy (&query);
   mongoc_cursor_destroy (cursor);
   mongoc_collection_destroy (collection);
   mongoc_client_destroy (client);

   return EXIT_SUCCESS;
}
