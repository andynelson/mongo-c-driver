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


#include "mongoc-client-private.h"
#include "mongoc-collection.h"
#include "mongoc-cursor.h"
#include "mongoc-cursor-private.h"
#include "mongoc-database.h"
#include "mongoc-database-private.h"
#include "mongoc-error.h"
#include "mongoc-log.h"
#include "mongoc-trace.h"
#include "mongoc-util-private.h"


#undef MONGOC_LOG_DOMAIN
#define MONGOC_LOG_DOMAIN "database"


/*
 *--------------------------------------------------------------------------
 *
 * _mongoc_database_new --
 *
 *       Create a new instance of mongoc_database_t for @client.
 *
 *       @client must stay valid for the life of the resulting
 *       database structure.
 *
 * Returns:
 *       A newly allocated mongoc_database_t that should be freed with
 *       mongoc_database_destroy().
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

mongoc_database_t *
_mongoc_database_new (mongoc_client_t              *client,
                      const char                   *name,
                      const mongoc_read_prefs_t    *read_prefs,
                      const mongoc_write_concern_t *write_concern)
{
   mongoc_database_t *db;

   ENTRY;

   bson_return_val_if_fail(client, NULL);
   bson_return_val_if_fail(name, NULL);

   db = bson_malloc0(sizeof *db);
   db->client = client;
   db->write_concern = write_concern ?
      mongoc_write_concern_copy(write_concern) :
      mongoc_write_concern_new();
   db->read_prefs = read_prefs ?
      mongoc_read_prefs_copy(read_prefs) :
      mongoc_read_prefs_new(MONGOC_READ_PRIMARY);

   strncpy(db->name, name, sizeof db->name);
   db->name[sizeof db->name-1] = '\0';

   RETURN(db);
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_database_destroy --
 *
 *       Releases resources associated with @database.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       Everything.
 *
 *--------------------------------------------------------------------------
 */

void
mongoc_database_destroy (mongoc_database_t *database)
{
   ENTRY;

   bson_return_if_fail(database);

   if (database->read_prefs) {
      mongoc_read_prefs_destroy(database->read_prefs);
      database->read_prefs = NULL;
   }

   if (database->write_concern) {
      mongoc_write_concern_destroy(database->write_concern);
      database->write_concern = NULL;
   }

   bson_free(database);

   EXIT;
}


mongoc_cursor_t *
mongoc_database_command (mongoc_database_t         *database,
                         mongoc_query_flags_t       flags,
                         bson_uint32_t              skip,
                         bson_uint32_t              limit,
                         bson_uint32_t              batch_size,
                         const bson_t              *command,
                         const bson_t              *fields,
                         const mongoc_read_prefs_t *read_prefs)
{
   BSON_ASSERT (database);
   BSON_ASSERT (command);

   if (!read_prefs) {
      read_prefs = database->read_prefs;
   }

   return mongoc_client_command (database->client, database->name, flags, skip,
                                 limit, batch_size, command, fields, read_prefs);
}


bson_bool_t
mongoc_database_command_simple (mongoc_database_t         *database,
                                const bson_t              *command,
                                const mongoc_read_prefs_t *read_prefs,
                                bson_t                    *reply,
                                bson_error_t              *error)
{
   BSON_ASSERT (database);
   BSON_ASSERT (command);

   if (!read_prefs) {
      read_prefs = database->read_prefs;
   }

   return mongoc_client_command_simple (database->client, database->name,
                                        command, read_prefs, reply, error);
}

/*
 *--------------------------------------------------------------------------
 *
 * mongoc_database_drop --
 *
 *       Requests that the MongoDB server drops @database, including all
 *       collections and indexes associated with @database.
 *
 *       Make sure this is really what you want!
 *
 * Returns:
 *       TRUE if @database was dropped.
 *
 * Side effects:
 *       @error may be set.
 *
 *--------------------------------------------------------------------------
 */

bson_bool_t
mongoc_database_drop (mongoc_database_t *database,
                      bson_error_t      *error)
{
   bson_bool_t ret;
   bson_t cmd;

   bson_return_val_if_fail(database, FALSE);

   bson_init(&cmd);
   bson_append_int32(&cmd, "dropDatabase", 12, 1);
   ret = mongoc_database_command_simple(database, &cmd, NULL, NULL, error);
   bson_destroy(&cmd);

   return ret;
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_database_add_user_legacy --
 *
 *       A helper to add a user or update their password on @database.
 *       This uses the legacy protocol by inserting into system.users.
 *
 * Returns:
 *       TRUE if successful; otherwise FALSE and @error is set.
 *
 * Side effects:
 *       @error may be set.
 *
 *--------------------------------------------------------------------------
 */

static bson_bool_t
mongoc_database_add_user_legacy (mongoc_database_t *database,
                                 const char        *username,
                                 const char        *password,
                                 bson_error_t      *error)
{
   mongoc_collection_t *collection;
   mongoc_cursor_t *cursor = NULL;
   const bson_t *doc;
   bson_bool_t ret = FALSE;
   bson_t query;
   bson_t user;
   char *input;
   char *pwd = NULL;

   bson_return_val_if_fail(database, FALSE);
   bson_return_val_if_fail(username, FALSE);
   bson_return_val_if_fail(password, FALSE);

   /*
    * Users are stored in the <dbname>.system.users virtual collection.
    * However, this will likely change to a command soon.
    */
   collection = mongoc_client_get_collection(database->client,
                                             database->name,
                                             "system.users");
   BSON_ASSERT(collection);

   /*
    * Hash the users password.
    */
   input = bson_strdup_printf("%s:mongo:%s", username, password);
   pwd = _mongoc_hex_md5(input);
   bson_free(input);

   /*
    * Check to see if the user exists. If so, we will update the
    * password instead of inserting a new user.
    */
   bson_init(&query);
   bson_append_utf8(&query, "user", 4, username, -1);
   cursor = mongoc_collection_find(collection, MONGOC_QUERY_NONE, 0, 1, 0,
                                   &query, NULL, NULL);
   if (!mongoc_cursor_next(cursor, &doc)) {
      if (mongoc_cursor_error(cursor, error)) {
         goto failure;
      }
      bson_init(&user);
      bson_append_utf8(&user, "user", 4, username, -1);
      bson_append_bool(&user, "readOnly", 8, FALSE);
      bson_append_utf8(&user, "pwd", 3, pwd, -1);
   } else {
      bson_copy_to_excluding(doc, &user, "pwd", (char *)NULL);
      bson_append_utf8(&user, "pwd", 3, pwd, -1);
   }

   if (!mongoc_collection_save(collection, &user, NULL, error)) {
      goto failure_with_user;
   }

   ret = TRUE;

failure_with_user:
   bson_destroy(&user);

failure:
   if (cursor) {
      mongoc_cursor_destroy(cursor);
   }
   mongoc_collection_destroy(collection);
   bson_destroy(&query);
   bson_free(pwd);

   return ret;
}


/**
 * mongoc_database_add_user:
 * @database: A #mongoc_database_t.
 * @username: A string containing the username.
 * @password: (allow-none): A string containing password, or NULL.
 * @roles: (allow-none): An optional bson_t of roles.
 * @custom_data: (allow-none): An optional bson_t of data to store.
 * @error: (out) (allow-none): A location for a bson_error_t or %NULL.
 *
 * Creates a new user with access to @database.
 *
 * Returns: None.
 * Side effects: None.
 */
bson_bool_t
mongoc_database_add_user (mongoc_database_t *database,
                          const char        *username,
                          const char        *password,
                          const bson_t      *roles,
                          const bson_t      *custom_data,
                          bson_error_t      *error)
{
   bson_bool_t ret = FALSE;

   ENTRY;

   BSON_ASSERT (database);
   BSON_ASSERT (username);

   /*
    * TODO: CDRIVER-232
    *
    * We need to first submit a command to the target system to manipulate
    * the user using commands. If the command fails, we fallback to legacy
    * user manipulation.
    */

   ret = mongoc_database_add_user_legacy (database, username, password, error);

   RETURN (ret);
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_database_get_read_prefs --
 *
 *       Fetch the read preferences for @database.
 *
 * Returns:
 *       A mongoc_read_prefs_t that should not be modified or freed.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

const mongoc_read_prefs_t *
mongoc_database_get_read_prefs (const mongoc_database_t *database) /* IN */
{
   bson_return_val_if_fail(database, NULL);
   return database->read_prefs;
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_database_set_read_prefs --
 *
 *       Sets the default read preferences for @database.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

void
mongoc_database_set_read_prefs (mongoc_database_t         *database,
                                const mongoc_read_prefs_t *read_prefs)
{
   bson_return_if_fail(database);

   if (database->read_prefs) {
      mongoc_read_prefs_destroy(database->read_prefs);
      database->read_prefs = NULL;
   }

   if (read_prefs) {
      database->read_prefs = mongoc_read_prefs_copy(read_prefs);
   }
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_database_get_write_concern --
 *
 *       Fetches the write concern for @database.
 *
 * Returns:
 *       A mongoc_write_concern_t that should not be modified or freed.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

const mongoc_write_concern_t *
mongoc_database_get_write_concern (const mongoc_database_t *database)
{
   bson_return_val_if_fail(database, NULL);

   return database->write_concern;
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_database_set_write_concern --
 *
 *       Set the default write concern for @database.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

void
mongoc_database_set_write_concern (mongoc_database_t            *database,
                                   const mongoc_write_concern_t *write_concern)
{
   bson_return_if_fail(database);

   if (database->write_concern) {
      mongoc_write_concern_destroy(database->write_concern);
      database->write_concern = NULL;
   }

   if (write_concern) {
      database->write_concern = mongoc_write_concern_copy(write_concern);
   }
}


/**
 * mongoc_database_has_collection:
 * @database: (in): A #mongoc_database_t.
 * @name: (in): The name of the collection to check for.
 * @error: (out) (allow-none): A location for a #bson_error_t, or %NULL.
 *
 * Checks to see if a collection exists within the database on the MongoDB
 * server.
 *
 * This will return %FALSE if their was an error communicating with the
 * server, or if the collection does not exist.
 *
 * If @error is provided, it will first be zeroed. Upon error, error.domain
 * will be set.
 *
 * Returns: %TRUE if @name exists, otherwise %FALSE. @error may be set.
 */
bson_bool_t
mongoc_database_has_collection (mongoc_database_t *database,
                                const char        *name,
                                bson_error_t      *error)
{
   mongoc_collection_t *collection;
   mongoc_read_prefs_t *read_prefs;
   mongoc_cursor_t *cursor;
   const bson_t *doc;
   bson_iter_t iter;
   bson_bool_t ret = FALSE;
   const char *cur_name;
   bson_t q = BSON_INITIALIZER;
   char ns[140];

   ENTRY;

   BSON_ASSERT (database);
   BSON_ASSERT (name);

   if (error) {
      memset (error, 0, sizeof *error);
   }

   snprintf (ns, sizeof ns, "%s.%s", database->name, name);
   ns[sizeof ns - 1] = '\0';

   read_prefs = mongoc_read_prefs_new (MONGOC_READ_PRIMARY);
   collection = mongoc_client_get_collection (database->client,
                                              database->name,
                                              "system.namespaces");
   cursor = mongoc_collection_find (collection, MONGOC_QUERY_NONE, 0, 0, 0, &q,
                                    NULL, read_prefs);

   while (!mongoc_cursor_error (cursor, error) &&
          mongoc_cursor_more (cursor)) {
      while (mongoc_cursor_next (cursor, &doc) &&
          bson_iter_init_find (&iter, doc, "name") &&
          BSON_ITER_HOLDS_UTF8 (&iter)) {
         cur_name = bson_iter_utf8(&iter, NULL);
         if (!strcmp(cur_name, ns)) {
            ret = TRUE;
            GOTO(cleanup);
         }
      }
   }

cleanup:
   mongoc_cursor_destroy (cursor);
   mongoc_collection_destroy (collection);
   mongoc_read_prefs_destroy (read_prefs);

   RETURN(ret);
}
