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


#ifndef MONGOC_CLIENT_PRIVATE_H
#define MONGOC_CLIENT_PRIVATE_H


#include <bson.h>

#include "mongoc-client.h"
#include "mongoc-event-private.h"


BSON_BEGIN_DECLS


bson_bool_t
mongoc_client_send (mongoc_client_t *client,
                    mongoc_event_t  *event,
                    bson_error_t    *error);


bson_bool_t
mongoc_client_recv (mongoc_client_t *client,
                    mongoc_event_t  *event,
                    bson_error_t    *error);


BSON_END_DECLS


#endif /* MONGOC_CLIENT_PRIVATE_H */