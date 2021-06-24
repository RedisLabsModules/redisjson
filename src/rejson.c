/*
* Copyright 2017-2019 Redis Labs Ltd. and Contributors
*
* This file is available under the Redis Labs Source Available License Agreement
*/
#include "rejson.h"
#include "cache.h"

// A struct to keep module the module context
typedef struct {
    JSONObjectCtx *joctx;
} ModuleCtx;
static ModuleCtx JSONCtx;

// == Helpers ==
#define NODEVALUE_AS_DOUBLE(n) (N_INTEGER == n->type ? (double)n->value.intval : n->value.numval)
#define NODETYPE(n) (n ? n->type : N_NULL)
struct JSONPathNode_t;
static void maybeClearPathCache(JSONType_t *jt, const struct JSONPathNode_t *pn);
/* Returns the string representation of a the node's type. */
static inline char *NodeTypeStr(const NodeType nt) {
    static char *types[] = {"null", "boolean", "integer", "number", "string", "object", "array"};
    switch (nt) {
        case N_NULL:
            return types[0];
        case N_BOOLEAN:
            return types[1];
        case N_INTEGER:
            return types[2];
        case N_NUMBER:
            return types[3];
        case N_STRING:
            return types[4];
        case N_DICT:
            return types[5];
        case N_ARRAY:
            return types[6];
        case N_KEYVAL:
            return NULL;  // this **should** never be reached
    }
    return NULL;  // this is never reached
}

/* Check if a search path is the root search path. */
static inline int SearchPath_IsRootPath(const SearchPath *sp) {
    return (1 == sp->len && NT_ROOT == sp->nodes[0].type);
}

/* Stores everything about a resolved path. */
typedef struct JSONPathNode_t {
    const char *spath;   // the path's string
    size_t spathlen;     // the path's string length
    Node *n;             // the referenced node
    Node *p;             // its parent
    SearchPath sp;       // the search path
    char *sperrmsg;      // the search path error message
    size_t sperroffset;  // the search path error offset
    PathError err;       // set in case of path error
    int errlevel;        // indicates the level of the error in the path
} JSONPathNode_t;

/* Call this to free the struct's contents. */
void JSONPathNode_Free(JSONPathNode_t *jpn) {
    if (jpn) {
        SearchPath_Free(&jpn->sp);
        RedisModule_Free(jpn);
    }
}

/* Sets n to the target node by path.
 * p is n's parent, errors are set into err and level is the error's depth
 * Returns PARSE_OK if parsing successful
 */
int NodeFromJSONPath(Node *root, const RedisModuleString *path, JSONPathNode_t **jpn) {
    // initialize everything
    JSONPathNode_t *_jpn = RedisModule_Calloc(1, sizeof(JSONPathNode_t));
    _jpn->errlevel = -1;
    JSONSearchPathError_t jsperr = {0};

    // path must be valid from the root or it's an error
    _jpn->sp = NewSearchPath(0);
    _jpn->spath = RedisModule_StringPtrLen(path, &_jpn->spathlen);
    if (PARSE_ERR == ParseJSONPath(_jpn->spath, _jpn->spathlen, &_jpn->sp, &jsperr)) {
        SearchPath_Free(&_jpn->sp);
        _jpn->sp.nodes = NULL;  // in case someone tries to free it later
        _jpn->sperrmsg = jsperr.errmsg;
        _jpn->sperroffset = jsperr.offset;
        *jpn = _jpn;
        return PARSE_ERR;
    }

    // if there are any errors return them
    if (!SearchPath_IsRootPath(&_jpn->sp)) {
        _jpn->err = SearchPath_FindEx(&_jpn->sp, root, &_jpn->n, &_jpn->p, &_jpn->errlevel);
    } else {
        // deal with edge case of setting root's parent
        _jpn->n = root;
    }

    *jpn = _jpn;
    return PARSE_OK;
}

/* Replies with an error about a search path */
void ReplyWithSearchPathError(RedisModuleCtx *ctx, JSONPathNode_t *jpn) {
    sds err = sdscatfmt(sdsempty(), "ERR Search path error at offset %I: %s",
                        (long long)jpn->sperroffset + 1, jpn->sperrmsg ? jpn->sperrmsg : "(null)");
    RedisModule_ReplyWithError(ctx, err);
    sdsfree(err);
}

/* Replies with an error about a wrong type of node in a path */
void ReplyWithPathTypeError(RedisModuleCtx *ctx, NodeType expected, NodeType actual) {
    sds err = sdscatfmt(sdsempty(), REJSON_ERROR_PATH_WRONGTYPE, NodeTypeStr(expected),
                        NodeTypeStr(actual));
    RedisModule_ReplyWithError(ctx, err);
    sdsfree(err);
}

/* Generic path error reply handler */
void ReplyWithPathError(RedisModuleCtx *ctx, const JSONPathNode_t *jpn) {
    // TODO: report actual position in path & literal token
    PathNode *epn = &jpn->sp.nodes[jpn->errlevel];
    sds err = sdsempty();
    switch (jpn->err) {
        case E_OK:
            err = sdscat(err, "ERR nothing wrong with path");
            break;
        case E_BADTYPE:
            if (NT_KEY == epn->type) {
                err = sdscatfmt(err, "ERR invalid key '[\"%s\"]' at level %i in path",
                                epn->value.key, jpn->errlevel);
            } else {
                err = sdscatfmt(err, "ERR invalid index '[%i]' at level %i in path",
                                epn->value.index, jpn->errlevel);
            }
            break;
        case E_NOINDEX:
            err = sdscatfmt(err, "ERR index '[%i]' out of range at level %i in path",
                            epn->value.index, jpn->errlevel);
            break;
        case E_NOKEY:
            err = sdscatfmt(err, "ERR key '%s' does not exist at level %i in path", epn->value.key,
                            jpn->errlevel);
            break;
        default:
            err = sdscatfmt(err, "ERR unknown path error at level %i in path", jpn->errlevel);
            break;
    }  // switch (err)
    RedisModule_ReplyWithError(ctx, err);
    sdsfree(err);
}

/* The custom Redis data type. */
static RedisModuleType *JSONType;

// == Module JSON commands ==

/**
* JSON.RESP <key> [path]
* Return the JSON in `key` in RESP.
*
* `path` defaults to root if not provided.
* This command uses the following mapping from JSON to RESP:
* - JSON Null is mapped to the RESP Null Bulk String
* - JSON `false` and `true` values are mapped to the respective RESP Simple Strings
* - JSON Numbers are mapped to RESP Integers or RESP Bulk Strings, depending on type
* - JSON Strings are mapped to RESP Bulk Strings
* - JSON Arrays are represented as RESP Arrays in which first element is the simple string `[`
*   followed by the array's elements
* - JSON Objects are represented as RESP Arrays in which first element is the simple string `{`.
    Each successive entry represents a key-value pair as a two-entries array of bulk strings.
*
* Reply: Array, specifically the JSON's RESP form.
*/
int JSONResp_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if ((argc < 2) || (argc > 3)) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }
    RedisModule_AutoMemory(ctx);

    // key must be empty (reply with null) or a JSON type
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    int type = RedisModule_KeyType(key);
    if (REDISMODULE_KEYTYPE_EMPTY == type) {
        RedisModule_ReplyWithNull(ctx);
        return REDISMODULE_OK;
    } else if (RedisModule_ModuleTypeGetType(key) != JSONType) {
        {
            RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
            return REDISMODULE_ERR;
        }
    }

    // validate path
    JSONType_t *jt = RedisModule_ModuleTypeGetValue(key);
    JSONPathNode_t *jpn = NULL;
    RedisModuleString *spath =
        (3 == argc ? argv[2] : RedisModule_CreateString(ctx, OBJECT_ROOT_PATH, 1));
    if (PARSE_OK != NodeFromJSONPath(jt->root, spath, &jpn)) {
        ReplyWithSearchPathError(ctx, jpn);
        goto error;
    }

    if (E_OK == jpn->err) {
        ObjectTypeToRespReply(ctx, jpn->n);
    } else {
        ReplyWithPathError(ctx, jpn);
        goto error;
    }

    JSONPathNode_Free(jpn);
    return REDISMODULE_OK;

error:
    JSONPathNode_Free(jpn);
    return REDISMODULE_ERR;
}

/**
 * JSON.DEBUG <subcommand & arguments>
 * Report information.
 *
 * Supported subcommands are:
 *   `MEMORY <key> [path]` - report the memory usage in bytes of a value. `path` defaults to root if
 *   not provided.
 *  `HELP` - replies with a helpful message
 *
 * Reply: depends on the subcommand used:
 *   `MEMORY` returns an integer, specifically the size in bytes of the value
 *   `HELP` returns an array, specifically with the help message
 */
int JSONDebug_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    // check for minimal arity
    if (argc < 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }
    RedisModule_AutoMemory(ctx);

    size_t subcmdlen;
    const char *subcmd = RedisModule_StringPtrLen(argv[1], &subcmdlen);
    if (!strncasecmp("memory", subcmd, subcmdlen)) {
        // verify we have enough arguments
        if ((argc < 3) || (argc > 4)) {
            RedisModule_WrongArity(ctx);
            return REDISMODULE_ERR;
        }

        // reply to getkeys-api requests
        if (RedisModule_IsKeysPositionRequest(ctx)) {
            RedisModule_KeyAtPos(ctx, 2);
            return REDISMODULE_OK;
        }

        // key must be empty (reply with null) or a JSON type
        RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[2], REDISMODULE_READ);
        int type = RedisModule_KeyType(key);
        if (REDISMODULE_KEYTYPE_EMPTY == type) {
            RedisModule_ReplyWithNull(ctx);
            return REDISMODULE_OK;
        } else if (RedisModule_ModuleTypeGetType(key) != JSONType) {
            RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
            return REDISMODULE_ERR;
        }

        // validate path
        JSONType_t *jt = RedisModule_ModuleTypeGetValue(key);
        JSONPathNode_t *jpn = NULL;
        RedisModuleString *spath =
            (4 == argc ? argv[3] : RedisModule_CreateString(ctx, OBJECT_ROOT_PATH, 1));
        if (PARSE_OK != NodeFromJSONPath(jt->root, spath, &jpn)) {
            ReplyWithSearchPathError(ctx, jpn);
            JSONPathNode_Free(jpn);
            return REDISMODULE_ERR;
        }

        if (E_OK == jpn->err) {
            RedisModule_ReplyWithLongLong(ctx, (long long)ObjectTypeMemoryUsage(jpn->n));
            JSONPathNode_Free(jpn);
            return REDISMODULE_OK;
        } else {
            ReplyWithPathError(ctx, jpn);
            JSONPathNode_Free(jpn);
            return REDISMODULE_ERR;
        }
    } else if (!strncasecmp("help", subcmd, subcmdlen)) {
        const char *help[] = {"MEMORY <key> [path] - reports memory usage",
                              "HELP                - this message", NULL};

        RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);
        int i = 0;
        for (; NULL != help[i]; i++) {
            RedisModule_ReplyWithStringBuffer(ctx, help[i], strlen(help[i]));
        }
        RedisModule_ReplySetArrayLength(ctx, i);

        return REDISMODULE_OK;
    } else {  // unknown subcommand
        RedisModule_ReplyWithError(ctx, "ERR unknown subcommand - try `JSON.DEBUG HELP`");
        return REDISMODULE_ERR;
    }
    return REDISMODULE_OK;  // this is never reached
}

/**
 * JSON.TYPE <key> [path]
 * Reports the type of JSON value at `path`.
 * `path` defaults to root if not provided. If the `key` or `path` do not exist, null is returned.
 * Reply: Simple string, specifically the type.
 */
int JSONType_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    // check args
    if ((argc < 2) || (argc > 3)) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }
    RedisModule_AutoMemory(ctx);

    // key must be empty or a JSON type
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    int type = RedisModule_KeyType(key);
    if (REDISMODULE_KEYTYPE_EMPTY == type) {
        RedisModule_ReplyWithNull(ctx);
        return REDISMODULE_OK;
    }
    if (RedisModule_ModuleTypeGetType(key) != JSONType) {
        RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
        return REDISMODULE_ERR;
    }

    // validate path
    JSONType_t *jt = RedisModule_ModuleTypeGetValue(key);
    JSONPathNode_t *jpn = NULL;
    RedisModuleString *spath =
        (3 == argc ? argv[2] : RedisModule_CreateString(ctx, OBJECT_ROOT_PATH, 1));
    if (PARSE_OK != NodeFromJSONPath(jt->root, spath, &jpn)) {
        ReplyWithSearchPathError(ctx, jpn);
        JSONPathNode_Free(jpn);
        return REDISMODULE_ERR;
    }

    // make the type-specifc reply, or deal with path errors
    if (E_OK == jpn->err) {
        RedisModule_ReplyWithSimpleString(ctx, NodeTypeStr(NODETYPE(jpn->n)));
    } else {
        // reply with null if there are **any** non-existing elements along the path
        RedisModule_ReplyWithNull(ctx);
    }

    JSONPathNode_Free(jpn);
    return REDISMODULE_OK;
}

/**
 * JSON.ARRLEN <key> [path]
 * JSON.OBJLEN <key> [path]
 * JSON.STRLEN <key> [path]
 * Report the length of the JSON value at `path` in `key`.
 *
 * `path` defaults to root if not provided. If the `key` or `path` do not exist, null is returned.
 *
 * Reply: Integer, specifically the length of the value.
 */
int JSONLen_GenericCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    // check args
    if ((argc < 2) || (argc > 3)) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }
    RedisModule_AutoMemory(ctx);

    // the actual command
    const char *cmd = RedisModule_StringPtrLen(argv[0], NULL);

    // key must be empty or a JSON type
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    int type = RedisModule_KeyType(key);
    if (REDISMODULE_KEYTYPE_EMPTY == type) {
        RedisModule_ReplyWithNull(ctx);
        return REDISMODULE_OK;
    }
    if (RedisModule_ModuleTypeGetType(key) != JSONType) {
        RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
        return REDISMODULE_ERR;
    }

    // validate path
    JSONType_t *jt = RedisModule_ModuleTypeGetValue(key);
    JSONPathNode_t *jpn = NULL;
    RedisModuleString *spath =
        (3 == argc ? argv[2] : RedisModule_CreateString(ctx, OBJECT_ROOT_PATH, 1));
    if (PARSE_OK != NodeFromJSONPath(jt->root, spath, &jpn)) {
        ReplyWithSearchPathError(ctx, jpn);
        goto error;
    }

    // deal with path errors
    if (E_OK != jpn->err) {
        ReplyWithPathError(ctx, jpn);
        goto error;
    }

    // determine the type of target value based on command name
    NodeType expected, actual = NODETYPE(jpn->n);
    if (!strcasecmp("json.arrlen", cmd))
        expected = N_ARRAY;
    else if (!strcasecmp("json.objlen", cmd))
        expected = N_DICT;
    else  // must be json.strlen
        expected = N_STRING;

    // reply with the length per type, or with an error if the wrong type is encountered
    if (actual == expected) {
        RedisModule_ReplyWithLongLong(ctx, Node_Length(jpn->n));
    } else {
        ReplyWithPathTypeError(ctx, expected, actual);
        goto error;
    }

    JSONPathNode_Free(jpn);
    return REDISMODULE_OK;

error:
    JSONPathNode_Free(jpn);
    return REDISMODULE_ERR;
}

/**
 * JSON.OBJKEYS <key> [path]
 * Return the keys in the object that's referenced by `path`.
 *
 * `path` defaults to root if not provided. If the object is empty, or either `key` or `path` do not
 * exist then null is returned.
 *
 * Reply: Array, specifically the key names as bulk strings.
 */
int JSONObjKeys_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    // check args
    if ((argc < 2) || (argc > 3)) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }
    RedisModule_AutoMemory(ctx);

    // key must be empty or a JSON type
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    int type = RedisModule_KeyType(key);
    if (REDISMODULE_KEYTYPE_EMPTY == type) {
        RedisModule_ReplyWithNull(ctx);
        return REDISMODULE_OK;
    }
    if (RedisModule_ModuleTypeGetType(key) != JSONType) {
        RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
        return REDISMODULE_ERR;
    }

    // validate path
    JSONType_t *jt = RedisModule_ModuleTypeGetValue(key);
    JSONPathNode_t *jpn = NULL;
    RedisModuleString *spath =
        (3 == argc ? argv[2] : RedisModule_CreateString(ctx, OBJECT_ROOT_PATH, 1));
    if (PARSE_OK != NodeFromJSONPath(jt->root, spath, &jpn)) {
        ReplyWithSearchPathError(ctx, jpn);
        goto error;
    }

    // deal with path errors
    if (E_NOINDEX == jpn->err || E_NOKEY == jpn->err) {
        // reply with null if there are **any** non-existing elements along the path
        RedisModule_ReplyWithNull(ctx);
        goto ok;
    } else if (E_OK != jpn->err) {
        ReplyWithPathError(ctx, jpn);
        goto error;
    }

    // reply with the object's keys if it is a dictionary, error otherwise
    if (N_DICT == NODETYPE(jpn->n)) {
        int len = Node_Length(jpn->n);
        RedisModule_ReplyWithArray(ctx, len);
        for (int i = 0; i < len; i++) {
            // TODO: need an iterator for keys in dict
            const char *k = jpn->n->value.dictval.entries[i]->value.kvval.key;
            RedisModule_ReplyWithStringBuffer(ctx, k, strlen(k));
        }
    } else {
        ReplyWithPathTypeError(ctx, N_DICT, NODETYPE(jpn->n));
        goto error;
    }

ok:
    JSONPathNode_Free(jpn);
    return REDISMODULE_OK;

error:
    JSONPathNode_Free(jpn);
    return REDISMODULE_ERR;
}

/**
 * JSON.SET <key> <path> <json> [NX|XX]
 * Sets the JSON value at `path` in `key`
 *
 * For new Redis keys the `path` must be the root. For existing keys, when the entire `path` exists,
 * the value that it contains is replaced with the `json` value.
 *
 * A key (with its respective value) is added to a JSON Object (in a Redis ReJSON data type key) if
 * and only if it is the last child in the `path`. The optional subcommands modify this behavior for
 * both new Redis ReJSON data type keys as well as JSON Object keys in them:
 *   `NX` - only set the key if it does not already exists
 *   `XX` - only set the key if it already exists
 *
 * Reply: Simple String `OK` if executed correctly, or Null Bulk if the specified `NX` or `XX`
 * conditions were not met.
 */
int JSONSet_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    // check args
    if ((argc < 4) || (argc > 5)) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }
    RedisModule_AutoMemory(ctx);

    // key must be empty or a JSON type
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
    int type = RedisModule_KeyType(key);
    if (REDISMODULE_KEYTYPE_EMPTY != type && RedisModule_ModuleTypeGetType(key) != JSONType) {
        RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
        return REDISMODULE_ERR;
    }

    JSONPathNode_t *jpn = NULL;
    Object *jo = NULL;
    char *jerr = NULL;

    // subcommand for key creation behavior modifiers NX and XX
    int subnx = 0, subxx = 0;
    if (argc > 4) {
        const char *subcmd = RedisModule_StringPtrLen(argv[4], NULL);
        if (!strcasecmp("nx", subcmd)) {
            subnx = 1;
        } else if (!strcasecmp("xx", subcmd)) {
            // new keys can be created only if the XX flag is off
            if (REDISMODULE_KEYTYPE_EMPTY == type) goto null;
            subxx = 1;
        } else {
            RedisModule_ReplyWithError(ctx, RM_ERRORMSG_SYNTAX);
            return REDISMODULE_ERR;
        }
    }

    // JSON must be valid
    size_t jsonlen;
    const char *json = RedisModule_StringPtrLen(argv[3], &jsonlen);
    if (!jsonlen) {
        RedisModule_ReplyWithError(ctx, REJSON_ERROR_EMPTY_STRING);
        return REDISMODULE_ERR;
    }

    // Create object from json
    if (JSONOBJECT_OK != CreateNodeFromJSON(JSONCtx.joctx, json, jsonlen, &jo, &jerr)) {
        if (jerr) {
            RedisModule_ReplyWithError(ctx, jerr);
            RedisModule_Free(jerr);
        } else {
            RM_LOG_WARNING(ctx, "%s", REJSON_ERROR_JSONOBJECT_ERROR);
            RedisModule_ReplyWithError(ctx, REJSON_ERROR_JSONOBJECT_ERROR);
        }
        return REDISMODULE_ERR;
    }

    // initialize or get JSON type container
    JSONType_t *jt = NULL;
    if (REDISMODULE_KEYTYPE_EMPTY == type) {
        jt = RedisModule_Calloc(1, sizeof(JSONType_t));
        jt->root = jo;
    } else {
        jt = RedisModule_ModuleTypeGetValue(key);
    }

    /* Validate path against the existing object root, and pretend that the new object is the root
     * if the key is empty. This will be caught immediately afterwards because new keys must be
     * created at the root.
     */
    if (PARSE_OK != NodeFromJSONPath(jt->root, argv[2], &jpn)) {
        ReplyWithSearchPathError(ctx, jpn);
        goto error;
    }
    int isRootPath = SearchPath_IsRootPath(&jpn->sp);

    // handle an empty key
    if (REDISMODULE_KEYTYPE_EMPTY == type) {
        // new keys must be created at the root
        if (E_OK != jpn->err || !isRootPath) {
            RedisModule_ReplyWithError(ctx, REJSON_ERROR_NEW_NOT_ROOT);
            goto error;
        }
        RedisModule_ModuleTypeSetValue(key, JSONType, jt);
        goto ok;
    }

    // handle an existing key, first make sure there weren't any obvious path errors
    if (E_OK != jpn->err && E_NOKEY != jpn->err) {
        ReplyWithPathError(ctx, jpn);
        goto error;
    }

    // verify that we're dealing with the last child in case of an object
    if (E_NOKEY == jpn->err && jpn->errlevel != jpn->sp.len - 1) {
        RedisModule_ReplyWithError(ctx, REJSON_ERROR_PATH_NONTERMINAL_KEY);
        goto error;
    }

    // replace a value according to its container type
    if (E_OK == jpn->err) {
        NodeType ntp = NODETYPE(jpn->p);

        // an existing value in the root or an object can be replaced only if the NX is off
        if (subnx && (isRootPath || N_DICT == ntp)) {
            goto null;
        }

        // other containers, i.e. arrays, do not sport the NX or XX behavioral modification agents
        if (N_ARRAY == ntp && (subnx || subxx)) {
            RedisModule_ReplyWithError(ctx, RM_ERRORMSG_SYNTAX);
            goto error;
        }

        if (isRootPath) {
            // replacing the root is easy
            RedisModule_DeleteKey(key);
            jt = RedisModule_Calloc(1, sizeof(JSONType_t));
            jt->root = jo;
            RedisModule_ModuleTypeSetValue(key, JSONType, jt);
        } else if (N_DICT == NODETYPE(jpn->p)) {
            if (OBJ_OK != Node_DictSet(jpn->p, jpn->sp.nodes[jpn->sp.len - 1].value.key, jo)) {
                RM_LOG_WARNING(ctx, "%s", REJSON_ERROR_DICT_SET);
                RedisModule_ReplyWithError(ctx, REJSON_ERROR_DICT_SET);
                goto error;
            }
        } else {  // must be an array
            int index = jpn->sp.nodes[jpn->sp.len - 1].value.index;
            if (index < 0) index = Node_Length(jpn->p) + index;
            if (OBJ_OK != Node_ArraySet(jpn->p, index, jo)) {
                RM_LOG_WARNING(ctx, "%s", REJSON_ERROR_ARRAY_SET);
                RedisModule_ReplyWithError(ctx, REJSON_ERROR_ARRAY_SET);
                goto error;
            }
            // unlike DictSet, ArraySet does not free so we need to call it explicitly
            Node_Free(jpn->n);
        }
    } else {  // must be E_NOKEY
        // new keys in the dictionary can be created only if the XX flag is off
        if (subxx) {
            goto null;
        }
        if (OBJ_OK != Node_DictSet(jpn->p, jpn->sp.nodes[jpn->sp.len - 1].value.key, jo)) {
            RM_LOG_WARNING(ctx, "%s", REJSON_ERROR_DICT_SET);
            RedisModule_ReplyWithError(ctx, REJSON_ERROR_DICT_SET);
            goto error;
        }
    }

ok:
    maybeClearPathCache(jt, jpn);
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    JSONPathNode_Free(jpn);
    RedisModule_ReplicateVerbatim(ctx);
    return REDISMODULE_OK;

null:
    RedisModule_ReplyWithNull(ctx);
    JSONPathNode_Free(jpn);
    if (jo) Node_Free(jo);
    return REDISMODULE_OK;

error:
    JSONPathNode_Free(jpn);
    if (jt && REDISMODULE_KEYTYPE_EMPTY == type) {
        RedisModule_Free(jt);
    }
    if (jo) Node_Free(jo);
    return REDISMODULE_ERR;
}

static void maybeClearPathCache(JSONType_t *jt, const JSONPathNode_t *pn) {
    if (!jt->lruEntries) {
        return;
    }

    const char *pathStr = pn->spath;
    size_t pathLen = pn->spathlen;
    if (pn->sp.hasLeadingDot) {
        pathStr++;
        pathLen--;
    }

    if (pathLen == 0) {
        LruCache_ClearKey(REJSON_LRUCACHE_GLOBAL, jt);
    } else {
        LruCache_ClearValues(REJSON_LRUCACHE_GLOBAL, jt, pathStr, pathLen);
    }
}

static sds getSerializedJson(JSONType_t *jt, const JSONPathNode_t *pathInfo,
                             const JSONSerializeOpt *opts, int *wasFound, sds *target) {
    // printf("Requesting value for path %.*s\n", (int)pathLen, path);

    // Normalize the path. If the original path begins with a dot, strip it
    const char *pathStr = pathInfo->spath;
    size_t pathLen = pathInfo->spathlen;
    int shouldCache = 1;
    sds ret = NULL;

    if (pathInfo->sp.hasLeadingDot) {
        pathStr++;
        pathLen--;
    }

    if (pathInfo->n) {
        switch (pathInfo->n->type) {
            // Don't store trivial types in the cache - i.e. those which aren't
            // costly to serialize.
            case N_NULL:
            case N_BOOLEAN:
            case N_INTEGER:
            case N_NUMBER:
                shouldCache = 0;
                break;
            default:
                shouldCache = 1;
                break;
        }
    } else {
        shouldCache = 0;
    }

    if (shouldCache) {
        ret = LruCache_GetValue(REJSON_LRUCACHE_GLOBAL, jt, pathStr, pathLen);
    }
    if (ret) {
        *wasFound = 1;
        if (target) {
            *target = sdscatsds(*target, ret);
        }
        return ret;
    }

    // Otherwise, serialize
    if (target) {
        ret = *target;
    } else {
        ret = sdsempty();
    }
    SerializeNodeToJSON(pathInfo->n, opts, &ret);
    if (shouldCache) {
        LruCache_AddValue(REJSON_LRUCACHE_GLOBAL, jt, pathStr, pathLen, ret, sdslen(ret));
    }
    *wasFound = 0;
    if (target) {
        *target = ret;
    }
    return ret;
}

static int isCachableOptions(const JSONSerializeOpt *opts) {
    return (!opts->indentstr || *opts->indentstr == 0) &&
           (!opts->newlinestr || *opts->newlinestr == 0) &&
           (!opts->spacestr || *opts->spacestr) == 0 && (opts->noescape == 0);
}

static void sendSingleResponse(RedisModuleCtx *ctx, JSONType_t *jt, const JSONPathNode_t *pn,
                               const JSONSerializeOpt *options) {
    sds json = NULL;
    if (!isCachableOptions(options)) {
        json = sdsempty();
        SerializeNodeToJSON(pn->n, options, &json);
        RedisModule_ReplyWithStringBuffer(ctx, json, sdslen(json));
        sdsfree(json);
        return;
    }

    int isFromCache = 0;
    json = getSerializedJson(jt, pn, options, &isFromCache, NULL);
    // Send the response now
    RedisModule_ReplyWithStringBuffer(ctx, json, sdslen(json));
    if (!isFromCache) {
        sdsfree(json);
    }
}

static void sendMultiResponse(RedisModuleCtx *ctx, JSONType_t *jt, JSONPathNode_t **pns,
                              size_t npns, const JSONSerializeOpt *options) {
    sds json = NULL;
    if (!isCachableOptions(options)) {
        // Use legacy behavior
        json = sdsempty();
        Node *objReply = NewDictNode(npns);
        for (int i = 0; i < npns; i++) {
            // add the path to the reply only if it isn't there already
            Node *target;
            int ret = Node_DictGet(objReply, pns[i]->spath, &target);
            if (OBJ_ERR == ret) {
                Node_DictSet(objReply, pns[i]->spath, pns[i]->n);
            }
        }
        SerializeNodeToJSON(objReply, options, &json);
        RedisModule_ReplyWithStringBuffer(ctx, json, sdslen(json));
        sdsfree(json);

        // avoid removing the actual data by resetting the reply dict
        // TODO: need a non-freeing Del
        for (int i = 0; i < objReply->value.dictval.len; i++) {
            objReply->value.dictval.entries[i]->value.kvval.val = NULL;
        }
        Node_Free(objReply);
        return;
    }

    json = sdsempty();
    json = sdscat(json, "{");
    for (int i = 0; i < npns; i++) {
        json = JSONSerialize_String(json, pns[i]->spath, pns[i]->spathlen, 1);
        json = sdscatlen(json, ":", 1);
        // Append to the buffer
        int dummy;
        getSerializedJson(jt, pns[i], options, &dummy, &json);
        if (i < npns - 1) {
            json = sdscat(json, ",");
        }
    }
    json = sdscat(json, "}");
    RedisModule_ReplyWithStringBuffer(ctx, json, sdslen(json));
    sdsfree(json);
}

/**
 * JSON.GET <key> [INDENT indentation-string] [NEWLINE newline-string] [SPACE space-string]
 *                [path ...]
 * Return the value at `path` in JSON serialized form.
 *
 * This command accepts multiple `path`s, and defaults to the value's root when none are given.
 *
 * The following subcommands change the reply's and are all set to the empty string by default:
 *   - `INDENT` sets the indentation string for nested levels
 *   - `NEWLINE` sets the string that's printed at the end of each line
 *   - `SPACE` sets the string that's put between a key and a value
 *   - `NOESCAPE` Don't escape any JSON characters.
 *
 * Reply: Bulk String, specifically the JSON serialization.
 * The reply's structure depends on the on the number of paths. A single path results in the
 * value being itself is returned, whereas multiple paths are returned as a JSON object in which
 * each path is a key.
 */
int JSONGet_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if ((argc < 2)) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }
    RedisModule_AutoMemory(ctx);

    // key must be empty (reply with null) or an object type
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    int type = RedisModule_KeyType(key);
    if (REDISMODULE_KEYTYPE_EMPTY == type) {
        RedisModule_ReplyWithNull(ctx);
        return REDISMODULE_OK;
    } else if (RedisModule_ModuleTypeGetType(key) != JSONType) {
        RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
        return REDISMODULE_ERR;
    }

    // check for optional arguments
    int pathpos = 2;
    JSONSerializeOpt jsopt = {0};
    if (pathpos < argc) {
        RMUtil_ParseArgsAfter("indent", argv, argc, "c", &jsopt.indentstr);
        if (jsopt.indentstr) {
            pathpos += 2;
        } else {
            jsopt.indentstr = "";
        }
    }
    if (pathpos < argc) {
        RMUtil_ParseArgsAfter("newline", argv, argc, "c", &jsopt.newlinestr);
        if (jsopt.newlinestr) {
            pathpos += 2;
        } else {
            jsopt.newlinestr = "";
        }
    }
    if (pathpos < argc) {
        RMUtil_ParseArgsAfter("space", argv, argc, "c", &jsopt.spacestr);
        if (jsopt.spacestr) {
            pathpos += 2;
        } else {
            jsopt.spacestr = "";
        }
    }

    if (RMUtil_ArgExists("noescape", argv, argc, 2)) {
        jsopt.noescape = 1;
        pathpos++;
    }

    // validate paths, if none provided default to root
    JSONType_t *jt = RedisModule_ModuleTypeGetValue(key);
    int npaths = argc - pathpos;
    int jpnslen = 0;
    JSONPathNode_t **jpns = RedisModule_Calloc(MAX(npaths, 1), sizeof(JSONPathNode_t *));
    if (!npaths) {  // default to root
        NodeFromJSONPath(jt->root, RedisModule_CreateString(ctx, OBJECT_ROOT_PATH, 1), &jpns[0]);
        jpnslen = 1;
    } else {
        while (jpnslen < npaths) {
            // validate path correctness
            if (PARSE_OK != NodeFromJSONPath(jt->root, argv[pathpos + jpnslen], &jpns[jpnslen])) {
                ReplyWithSearchPathError(ctx, jpns[jpnslen]);
                jpnslen++;
                goto error;
            }

            // deal with path errors
            if (E_OK != jpns[jpnslen]->err) {
                ReplyWithPathError(ctx, jpns[jpnslen]);
                jpnslen++;
                goto error;
            }

            jpnslen++;
        }  // while (jpnslen < npaths)
    }

    // return the single path's JSON value, or wrap all paths-values as an object
    if (1 == jpnslen) {
        sendSingleResponse(ctx, jt, jpns[0], &jsopt);
    } else {
        sendMultiResponse(ctx, jt, jpns, jpnslen, &jsopt);
    }

    // check whether serialization had succeeded
    // if (!sdslen(json)) {
    //     RM_LOG_WARNING(ctx, "%s", REJSON_ERROR_SERIALIZE);
    //     RedisModule_ReplyWithError(ctx, REJSON_ERROR_SERIALIZE);
    //     goto error;
    // }

    for (int i = 0; i < jpnslen; i++) {
        JSONPathNode_Free(jpns[i]);
    }

    RedisModule_Free(jpns);
    return REDISMODULE_OK;

error:
    for (int i = 0; i < jpnslen; i++) {
        JSONPathNode_Free(jpns[i]);
    }
    RedisModule_Free(jpns);
    return REDISMODULE_ERR;
}

/**
 * JSON.MGET <key> [<key> ...] <path>
 * Returns the values at `path` from multiple `key`s. Non-existing keys and non-existing paths
 * are reported as null. Reply: Array of Bulk Strings, specifically the JSON serialization of
 * the value at each key's path.
 */
int JSONMGet_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if ((argc < 2)) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }
    if (RedisModule_IsKeysPositionRequest(ctx)) {
        for (int i = 1; i < argc - 1; i++) RedisModule_KeyAtPos(ctx, i);
        return REDISMODULE_OK;
    }
    RedisModule_AutoMemory(ctx);

    // validate search path
    size_t spathlen;
    const char *spath = RedisModule_StringPtrLen(argv[argc - 1], &spathlen);
    JSONPathNode_t jpn = {0};
    JSONSearchPathError_t jsperr = {0};
    jpn.sp = NewSearchPath(0);
    if (PARSE_ERR == ParseJSONPath(spath, spathlen, &jpn.sp, &jsperr)) {
        jpn.sperrmsg = jsperr.errmsg;
        jpn.sperroffset = jsperr.offset;
        ReplyWithSearchPathError(ctx, &jpn);
        goto error;
    }

    // iterate keys
    RedisModule_ReplyWithArray(ctx, argc - 2);
    int isRootPath = SearchPath_IsRootPath(&jpn.sp);
    JSONSerializeOpt jsopt = {0};
    for (int i = 1; i < argc - 1; i++) {
        RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[i], REDISMODULE_READ);

        // key must an object type, empties and others return null like Redis' MGET
        int type = RedisModule_KeyType(key);
        if (REDISMODULE_KEYTYPE_EMPTY == type) goto null;
        if (RedisModule_ModuleTypeGetType(key) != JSONType) goto null;

        // follow the path to the target node in the key
        JSONType_t *jt = RedisModule_ModuleTypeGetValue(key);
        if (isRootPath) {
            jpn.err = E_OK;
            jpn.n = jt->root;
        } else {
            jpn.err = SearchPath_FindEx(&jpn.sp, jt->root, &jpn.n, &jpn.p, &jpn.errlevel);
        }

        // deal with path errors by returning null
        if (E_OK != jpn.err) goto null;

        // serialize it
        sds json = sdsempty();
        SerializeNodeToJSON(jpn.n, &jsopt, &json);

        // check whether serialization had succeeded
        if (!sdslen(json)) {
            sdsfree(json);
            RM_LOG_WARNING(ctx, "%s", REJSON_ERROR_SERIALIZE);
            RedisModule_ReplyWithError(ctx, REJSON_ERROR_SERIALIZE);
            goto error;
        }

        // add the serialization of object for that key's path
        RedisModule_ReplyWithStringBuffer(ctx, json, sdslen(json));
        sdsfree(json);
        continue;

    null:  // reply with null for keys that the path mismatches
        RedisModule_ReplyWithNull(ctx);
    }

    SearchPath_Free(&jpn.sp);
    return REDISMODULE_OK;

error:
    SearchPath_Free(&jpn.sp);
    return REDISMODULE_ERR;
}

/**
 * JSON.DEL <key> [path]
 * Delete a value.
 *
 * `path` defaults to root if not provided. Non-existing keys as well as non-existing paths are
 * ignored. Deleting an object's root is equivalent to deleting the key from Redis.
 *
 * Reply: Integer, specifically the number of paths deleted (0 or 1).
 */
int JSONDel_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    // check args
    if ((argc < 2) || (argc > 3)) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }
    RedisModule_AutoMemory(ctx);

    // key must be empty or a JSON type
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
    int type = RedisModule_KeyType(key);
    if (REDISMODULE_KEYTYPE_EMPTY == type) {
        RedisModule_ReplyWithLongLong(ctx, 0);
        return REDISMODULE_OK;
    } else if (RedisModule_ModuleTypeGetType(key) != JSONType) {
        RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
        return REDISMODULE_ERR;
    }

    // validate path
    JSONType_t *jt = RedisModule_ModuleTypeGetValue(key);
    JSONPathNode_t *jpn = NULL;
    RedisModuleString *spath =
        (3 == argc ? argv[2] : RedisModule_CreateString(ctx, OBJECT_ROOT_PATH, 1));
    if (PARSE_OK != NodeFromJSONPath(jt->root, spath, &jpn)) {
        ReplyWithSearchPathError(ctx, jpn);
        goto error;
    }

    // deal with path errors
    if (E_NOINDEX == jpn->err || E_NOKEY == jpn->err) {
        // reply with 0 if there are **any** non-existing elements along the path
        RedisModule_ReplyWithLongLong(ctx, 0);
        goto ok;
    } else if (E_OK != jpn->err) {
        ReplyWithPathError(ctx, jpn);
        goto error;
    }

    // Delete from the LRU cache, if needed
    maybeClearPathCache(jt, jpn);

    // if it is the root then delete the key, otherwise delete the target from parent container
    if (SearchPath_IsRootPath(&jpn->sp)) {
        RedisModule_DeleteKey(key);
    } else if (N_DICT == NODETYPE(jpn->p)) {  // delete from a dict
        const char *dictkey = jpn->sp.nodes[jpn->sp.len - 1].value.key;
        if (OBJ_OK != Node_DictDel(jpn->p, dictkey)) {
            RM_LOG_WARNING(ctx, "%s", REJSON_ERROR_DICT_DEL);
            RedisModule_ReplyWithError(ctx, REJSON_ERROR_DICT_DEL);
            goto error;
        }
    } else {  // container must be an array
        int index = jpn->sp.nodes[jpn->sp.len - 1].value.index;
        if (OBJ_OK != Node_ArrayDelRange(jpn->p, index, 1)) {
            RM_LOG_WARNING(ctx, "%s", REJSON_ERROR_ARRAY_DEL);
            RedisModule_ReplyWithError(ctx, REJSON_ERROR_ARRAY_DEL);
            goto error;
        }
    }  // if (N_DICT)

    RedisModule_ReplyWithLongLong(ctx, (long long)argc - 2);

ok:
    JSONPathNode_Free(jpn);
    RedisModule_ReplicateVerbatim(ctx);
    return REDISMODULE_OK;

error:
    JSONPathNode_Free(jpn);
    return REDISMODULE_ERR;
}

/**
 * JSON.NUMINCRBY <key> [path] <value>
 * JSON.NUMMULTBY <key> [path] <value>
 * Increments/multiplies the value stored under `path` by `value`.
 * `path` must exist path and must be a number value.
 * Reply: String, specifically the resulting JSON number value
 */
int JSONNum_GenericCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if ((argc < 3) || (argc > 4)) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }
    RedisModule_AutoMemory(ctx);

    const char *cmd = RedisModule_StringPtrLen(argv[0], NULL);
    double oval, bval, rz;  // original value, by value and the result
    Object *joval = NULL;   // the by value as a JSON object

    // key must be an object type
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
    int type = RedisModule_KeyType(key);
    if (REDISMODULE_KEYTYPE_EMPTY == type) {
        RedisModule_ReplyWithError(ctx, REJSON_ERROR_KEY_REQUIRED);
        return REDISMODULE_ERR;
    } else if (RedisModule_ModuleTypeGetType(key) != JSONType) {
        RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
        return REDISMODULE_ERR;
    }

    // validate path
    JSONType_t *jt = RedisModule_ModuleTypeGetValue(key);
    JSONPathNode_t *jpn = NULL;
    RedisModuleString *spath =
        (4 == argc ? argv[2] : RedisModule_CreateString(ctx, OBJECT_ROOT_PATH, 1));
    if (PARSE_OK != NodeFromJSONPath(jt->root, spath, &jpn)) {
        ReplyWithSearchPathError(ctx, jpn);
        goto error;
    }

    // deal with path errors
    if (E_OK != jpn->err) {
        ReplyWithPathError(ctx, jpn);
        goto error;
    }

    // verify that the target value is a number
    if (N_INTEGER != NODETYPE(jpn->n) && N_NUMBER != NODETYPE(jpn->n)) {
        sds err = sdscatfmt(sdsempty(), REJSON_ERROR_PATH_NANTYPE, NodeTypeStr(NODETYPE(jpn->n)));
        RedisModule_ReplyWithError(ctx, err);
        sdsfree(err);
        goto error;
    }
    oval = NODEVALUE_AS_DOUBLE(jpn->n);

    // we use the json parser to convert the bval arg into a value to catch all of JSON's
    // syntices
    size_t vallen;
    const char *val = RedisModule_StringPtrLen(argv[(4 == argc ? 3 : 2)], &vallen);
    char *jerr = NULL;
    if (JSONOBJECT_OK != CreateNodeFromJSON(JSONCtx.joctx, val, vallen, &joval, &jerr)) {
        if (jerr) {
            RedisModule_ReplyWithError(ctx, jerr);
            RedisModule_Free(jerr);
        } else {
            RM_LOG_WARNING(ctx, "%s", REJSON_ERROR_JSONOBJECT_ERROR);
            RedisModule_ReplyWithError(ctx, REJSON_ERROR_JSONOBJECT_ERROR);
        }
        goto error;
    }

    // the by value must be a number
    if (N_INTEGER != NODETYPE(joval) && N_NUMBER != NODETYPE(joval)) {
        RedisModule_ReplyWithError(ctx, REJSON_ERROR_VALUE_NAN);
        goto error;
    }
    bval = NODEVALUE_AS_DOUBLE(joval);

    // perform the operation
    if (!strcasecmp("json.numincrby", cmd)) {
        rz = oval + bval;
    } else {
        rz = oval * bval;
    }

    // check that the result is valid
    if (isnan(rz) || isinf(rz)) {
        RedisModule_ReplyWithError(ctx, REJSON_ERROR_RESULT_NAN_OR_INF);
        goto error;
    }

    // make an object out of the result per its type
    Object *orz;
    // the result is an integer only if both values were, and providing an int64 can hold it
    if (N_INTEGER == NODETYPE(jpn->n) && N_INTEGER == NODETYPE(joval) && rz <= (double)INT64_MAX &&
        rz >= (double)INT64_MIN) {
        orz = NewIntNode((int64_t)rz);
    } else {
        orz = NewDoubleNode(rz);
    }

    // replace the original value with the result depending on the parent container's type
    if (SearchPath_IsRootPath(&jpn->sp)) {
        RedisModule_DeleteKey(key);
        jt = RedisModule_Calloc(1, sizeof(JSONType_t));
        jt->root = orz;
        RedisModule_ModuleTypeSetValue(key, JSONType, jt);
    } else if (N_DICT == NODETYPE(jpn->p)) {
        if (OBJ_OK != Node_DictSet(jpn->p, jpn->sp.nodes[jpn->sp.len - 1].value.key, orz)) {
            RM_LOG_WARNING(ctx, "%s", REJSON_ERROR_DICT_SET);
            RedisModule_ReplyWithError(ctx, REJSON_ERROR_DICT_SET);
            goto error;
        }
    } else {  // container must be an array
        int index = jpn->sp.nodes[jpn->sp.len - 1].value.index;
        if (index < 0) index = Node_Length(jpn->p) + index;
        if (OBJ_OK != Node_ArraySet(jpn->p, index, orz)) {
            RM_LOG_WARNING(ctx, "%s", REJSON_ERROR_ARRAY_SET);
            RedisModule_ReplyWithError(ctx, REJSON_ERROR_ARRAY_SET);
            goto error;
        }
        // unlike DictSet, ArraySet does not free so we need to call it explicitly
        Node_Free(jpn->n);
    }
    jpn->n = orz;

    // reply with the serialization of the new value
    JSONSerializeOpt jsopt = {0};
    sds json = sdsempty();
    SerializeNodeToJSON(jpn->n, &jsopt, &json);
    RedisModule_ReplyWithStringBuffer(ctx, json, sdslen(json));
    maybeClearPathCache(jt, jpn);

    sdsfree(json);

    Node_Free(joval);
    JSONPathNode_Free(jpn);

    RedisModule_ReplicateVerbatim(ctx);
    return REDISMODULE_OK;

error:
    Node_Free(joval);
    JSONPathNode_Free(jpn);
    return REDISMODULE_ERR;
}

/**
 * JSON.STRAPPEND <key> [path] <json-string>
 * Append the `json-string` value(s) the string at `path`.
 * `path` defaults to root if not provided.
 * Reply: Integer, specifically the string's new length.
 */
int JSONStrAppend_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    // check args
    if ((argc < 3) || (argc > 4)) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }
    RedisModule_AutoMemory(ctx);

    // key can't be empty and must be a JSON type
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
    int type = RedisModule_KeyType(key);
    if (REDISMODULE_KEYTYPE_EMPTY == type || RedisModule_ModuleTypeGetType(key) != JSONType) {
        RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
        return REDISMODULE_ERR;
    }

    // validate path
    JSONType_t *jt = RedisModule_ModuleTypeGetValue(key);
    JSONPathNode_t *jpn = NULL;
    RedisModuleString *spath =
        (4 == argc ? argv[2] : RedisModule_CreateString(ctx, OBJECT_ROOT_PATH, 1));
    if (PARSE_OK != NodeFromJSONPath(jt->root, spath, &jpn)) {
        ReplyWithSearchPathError(ctx, jpn);
        goto error;
    }

    // deal with path errors
    if (E_OK != jpn->err) {
        ReplyWithPathError(ctx, jpn);
        goto error;
    }

    // the target must be a string
    if (N_STRING != NODETYPE(jpn->n)) {
        ReplyWithPathTypeError(ctx, N_STRING, NODETYPE(jpn->n));
        goto error;
    }

    // JSON must be valid
    size_t jsonlen;
    const char *json = RedisModule_StringPtrLen(argv[(4 == argc ? 3 : 2)], &jsonlen);
    if (!jsonlen) {
        RedisModule_ReplyWithError(ctx, REJSON_ERROR_EMPTY_STRING);
        goto error;
    }

    // make an object from the JSON value
    Object *jo = NULL;
    char *jerr = NULL;
    if (JSONOBJECT_OK != CreateNodeFromJSON(JSONCtx.joctx, json, jsonlen, &jo, &jerr)) {
        if (jerr) {
            RedisModule_ReplyWithError(ctx, jerr);
            RedisModule_Free(jerr);
        } else {
            RM_LOG_WARNING(ctx, "%s", REJSON_ERROR_JSONOBJECT_ERROR);
            RedisModule_ReplyWithError(ctx, REJSON_ERROR_JSONOBJECT_ERROR);
        }
        goto error;
    }

    // the value must be a string
    if (N_STRING != NODETYPE(jo)) {
        sds err = sdscatfmt(sdsempty(), "ERR wrong type of value - expected %s but found %s",
                            NodeTypeStr(N_STRING), NodeTypeStr(NODETYPE(jo)));
        RedisModule_ReplyWithError(ctx, err);
        sdsfree(err);
        Node_Free(jo);
        goto error;
    }

    // actually concatenate the strings
    Node_StringAppend(jpn->n, jo);
    maybeClearPathCache(jt, jpn);
    RedisModule_ReplyWithLongLong(ctx, (long long)Node_Length(jpn->n));
    Node_Free(jo);
    JSONPathNode_Free(jpn);

    RedisModule_ReplicateVerbatim(ctx);
    return REDISMODULE_OK;

error:
    JSONPathNode_Free(jpn);
    return REDISMODULE_ERR;
}

/**
 * JSON.ARRINSERT <key> <path> <index> <json> [<json> ...]
 * Insert the `json` value(s) into the array at `path` before the `index` (shifts to the right).
 *
 * The index must be in the array's range. Inserting at `index` 0 prepends to the array.
 * Negative index values are interpreted as starting from the end.
 *
 * Reply: Integer, specifically the array's new size
 */
int JSONArrInsert_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    // check args
    if (argc < 5) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }
    RedisModule_AutoMemory(ctx);

    // key can't be empty and must be a JSON type
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
    int type = RedisModule_KeyType(key);
    if (REDISMODULE_KEYTYPE_EMPTY == type || RedisModule_ModuleTypeGetType(key) != JSONType) {
        RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
        return REDISMODULE_ERR;
    }

    // validate path
    JSONType_t *jt = RedisModule_ModuleTypeGetValue(key);
    JSONPathNode_t *jpn = NULL;
    if (PARSE_OK != NodeFromJSONPath(jt->root, argv[2], &jpn)) {
        ReplyWithSearchPathError(ctx, jpn);
        goto error;
    }

    // deal with path errors
    if (E_OK != jpn->err) {
        ReplyWithPathError(ctx, jpn);
        goto error;
    }

    // the target must be an array
    if (N_ARRAY != NODETYPE(jpn->n)) {
        ReplyWithPathTypeError(ctx, N_ARRAY, NODETYPE(jpn->n));
        goto error;
    }

    // get the index
    long long index;
    if (REDISMODULE_OK != RedisModule_StringToLongLong(argv[3], &index)) {
        RedisModule_ReplyWithError(ctx, REJSON_ERROR_INDEX_INVALID);
        goto error;
    }

    // convert negative values
    if (index < 0) index = Node_Length(jpn->n) + index;

    // check for out of range
    if (index < 0 || index > Node_Length(jpn->n)) {
        RedisModule_ReplyWithError(ctx, REJSON_ERROR_INDEX_OUTOFRANGE);
        goto error;
    }

    // make an array from the JSON values
    Node *sub = NewArrayNode(argc - 4);
    for (int i = 4; i < argc; i++) {
        // JSON must be valid
        size_t jsonlen;
        const char *json = RedisModule_StringPtrLen(argv[i], &jsonlen);
        if (!jsonlen) {
            RedisModule_ReplyWithError(ctx, REJSON_ERROR_EMPTY_STRING);
            Node_Free(sub);
            goto error;
        }

        // create object from json
        Object *jo = NULL;
        char *jerr = NULL;
        if (JSONOBJECT_OK != CreateNodeFromJSON(JSONCtx.joctx, json, jsonlen, &jo, &jerr)) {
            Node_Free(sub);
            if (jerr) {
                RedisModule_ReplyWithError(ctx, jerr);
                RedisModule_Free(jerr);
            } else {
                RM_LOG_WARNING(ctx, "%s", REJSON_ERROR_JSONOBJECT_ERROR);
                RedisModule_ReplyWithError(ctx, REJSON_ERROR_JSONOBJECT_ERROR);
            }
            goto error;
        }

        // append it to the sub array
        if (OBJ_OK != Node_ArrayAppend(sub, jo)) {
            Node_Free(jo);
            Node_Free(sub);
            RM_LOG_WARNING(ctx, "%s", REJSON_ERROR_INSERT_SUBARRY);
            RedisModule_ReplyWithError(ctx, REJSON_ERROR_INSERT_SUBARRY);
            goto error;
        }
    }

    // insert the sub array to the target array
    if (OBJ_OK != Node_ArrayInsert(jpn->n, index, sub)) {
        Node_Free(sub);
        RM_LOG_WARNING(ctx, "%s", REJSON_ERROR_INSERT);
        RedisModule_ReplyWithError(ctx, REJSON_ERROR_INSERT);
        goto error;
    }

    RedisModule_ReplyWithLongLong(ctx, Node_Length(jpn->n));
    maybeClearPathCache(jt, jpn);
    JSONPathNode_Free(jpn);
    RedisModule_ReplicateVerbatim(ctx);
    return REDISMODULE_OK;

error:
    JSONPathNode_Free(jpn);
    return REDISMODULE_ERR;
}

/* JSON.ARRAPPEND <key> <path> <json> [<json> ...]
 * Append the `json` value(s) into the array at `path` after the last element in it.
 * Reply: Integer, specifically the array's new size
 */
int JSONArrAppend_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    // check args
    if (argc < 4) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }
    RedisModule_AutoMemory(ctx);

    // key can't be empty and must be a JSON type
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
    int type = RedisModule_KeyType(key);
    if (REDISMODULE_KEYTYPE_EMPTY == type || RedisModule_ModuleTypeGetType(key) != JSONType) {
        RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
        return REDISMODULE_ERR;
    }

    // validate path
    JSONType_t *jt = RedisModule_ModuleTypeGetValue(key);
    JSONPathNode_t *jpn = NULL;
    if (PARSE_OK != NodeFromJSONPath(jt->root, argv[2], &jpn)) {
        ReplyWithSearchPathError(ctx, jpn);
        goto error;
    }

    // deal with path errors
    if (E_OK != jpn->err) {
        ReplyWithPathError(ctx, jpn);
        goto error;
    }

    // the target must be an array
    if (N_ARRAY != NODETYPE(jpn->n)) {
        ReplyWithPathTypeError(ctx, N_ARRAY, NODETYPE(jpn->n));
        goto error;
    }

    // make an array from the JSON values
    Node *sub = NewArrayNode(argc - 3);
    for (int i = 3; i < argc; i++) {
        // JSON must be valid
        size_t jsonlen;
        const char *json = RedisModule_StringPtrLen(argv[i], &jsonlen);
        if (!jsonlen) {
            RedisModule_ReplyWithError(ctx, REJSON_ERROR_EMPTY_STRING);
            Node_Free(sub);
            goto error;
        }

        // create object from json
        Object *jo = NULL;
        char *jerr = NULL;
        if (JSONOBJECT_OK != CreateNodeFromJSON(JSONCtx.joctx, json, jsonlen, &jo, &jerr)) {
            Node_Free(sub);
            if (jerr) {
                RedisModule_ReplyWithError(ctx, jerr);
                RedisModule_Free(jerr);
            } else {
                RM_LOG_WARNING(ctx, "%s", REJSON_ERROR_JSONOBJECT_ERROR);
                RedisModule_ReplyWithError(ctx, REJSON_ERROR_JSONOBJECT_ERROR);
            }
            goto error;
        }

        // append it to the sub array
        if (OBJ_OK != Node_ArrayAppend(sub, jo)) {
            Node_Free(jo);
            Node_Free(sub);
            RM_LOG_WARNING(ctx, "%s", REJSON_ERROR_INSERT_SUBARRY);
            RedisModule_ReplyWithError(ctx, REJSON_ERROR_INSERT_SUBARRY);
            goto error;
        }
    }

    // insert the sub array to the target array
    if (OBJ_OK != Node_ArrayInsert(jpn->n, Node_Length(jpn->n), sub)) {
        Node_Free(sub);
        RM_LOG_WARNING(ctx, "%s", REJSON_ERROR_INSERT);
        RedisModule_ReplyWithError(ctx, REJSON_ERROR_INSERT);
        goto error;
    }

    RedisModule_ReplyWithLongLong(ctx, Node_Length(jpn->n));
    maybeClearPathCache(jt, jpn);
    JSONPathNode_Free(jpn);
    RedisModule_ReplicateVerbatim(ctx);
    return REDISMODULE_OK;

error:
    JSONPathNode_Free(jpn);
    return REDISMODULE_ERR;
}

/**
 * JSON.ARRINDEX <key> <path> <scalar> [start [stop]]
 * Search for the first occurance of a scalar JSON value in an array.
 *
 * The optional inclusive `start` (default 0) and exclusive `stop` (default 0, meaning that the
 * last element is included) specify a slice of the array to search.
 *
 * Note: out of range errors are treated by rounding the index to the array's start and end. An
 * inverse index range (e.g, from 1 to 0) will return unfound.
 *
 * Reply: Integer, specifically the position of the scalar value in the array or -1 if unfound.
 */
int JSONArrIndex_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    // check args
    if ((argc < 4) || (argc > 6)) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }
    RedisModule_AutoMemory(ctx);

    // key can't be empty and must be a JSON type
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    int type = RedisModule_KeyType(key);
    if (REDISMODULE_KEYTYPE_EMPTY == type || RedisModule_ModuleTypeGetType(key) != JSONType) {
        RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
        return REDISMODULE_ERR;
    }

    // validate path
    JSONType_t *jt = RedisModule_ModuleTypeGetValue(key);
    JSONPathNode_t *jpn = NULL;
    Object *jo = NULL;
    if (PARSE_OK != NodeFromJSONPath(jt->root, argv[2], &jpn)) {
        ReplyWithSearchPathError(ctx, jpn);
        goto error;
    }

    // deal with path errors
    if (E_OK != jpn->err) {
        ReplyWithPathError(ctx, jpn);
        goto error;
    }

    // verify that the target's type is an array
    if (N_ARRAY != NODETYPE(jpn->n)) {
        ReplyWithPathTypeError(ctx, N_ARRAY, NODETYPE(jpn->n));
        goto error;
    }

    // the JSON value to search for must be valid
    size_t jsonlen;
    const char *json = RedisModule_StringPtrLen(argv[3], &jsonlen);
    if (!jsonlen) {
        RedisModule_ReplyWithError(ctx, REJSON_ERROR_EMPTY_STRING);
        goto error;
    }

    // create an object from json
    char *jerr = NULL;
    if (JSONOBJECT_OK != CreateNodeFromJSON(JSONCtx.joctx, json, jsonlen, &jo, &jerr)) {
        if (jerr) {
            RedisModule_ReplyWithError(ctx, jerr);
            RedisModule_Free(jerr);
        } else {
            RM_LOG_WARNING(ctx, "%s", REJSON_ERROR_JSONOBJECT_ERROR);
            RedisModule_ReplyWithError(ctx, REJSON_ERROR_JSONOBJECT_ERROR);
        }
        goto error;
    }

    // get start (inclusive) & stop (exlusive) indices
    long long start = 0, stop = 0;
    if (argc > 4) {
        if (REDISMODULE_OK != RedisModule_StringToLongLong(argv[4], &start)) {
            RedisModule_ReplyWithError(ctx, REJSON_ERROR_INDEX_INVALID);
            goto error;
        }
        if (argc > 5) {
            if (REDISMODULE_OK != RedisModule_StringToLongLong(argv[5], &stop)) {
                RedisModule_ReplyWithError(ctx, REJSON_ERROR_INDEX_INVALID);
                goto error;
            }
        }
    }

    RedisModule_ReplyWithLongLong(ctx, Node_ArrayIndex(jpn->n, jo, (int)start, (int)stop));

    JSONPathNode_Free(jpn);
    Node_Free(jo);
    return REDISMODULE_OK;

error:
    JSONPathNode_Free(jpn);
    if (jo) Node_Free(jo);
    return REDISMODULE_ERR;
}

/**
 * JSON.ARRPOP <key> [path [index]]
 * Remove and return element from the index in the array.
 *
 * `path` defaults to root if not provided. `index` is the position in the array to start
 * popping from (defaults to -1, meaning the last element). Out of range indices are rounded to
 * their respective array ends. Popping an empty array yields null.
 *
 * Reply: Bulk String, specifically the popped JSON value.
 */
int JSONArrPop_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    // check args
    if ((argc < 2) || (argc > 4)) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }
    RedisModule_AutoMemory(ctx);

    // key can't be empty and must be a JSON type
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    int type = RedisModule_KeyType(key);
    if (REDISMODULE_KEYTYPE_EMPTY == type || RedisModule_ModuleTypeGetType(key) != JSONType) {
        RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
        return REDISMODULE_ERR;
    }

    // validate path
    JSONType_t *jt = RedisModule_ModuleTypeGetValue(key);
    JSONPathNode_t *jpn = NULL;
    RedisModuleString *spath =
        (argc > 2 ? argv[2] : RedisModule_CreateString(ctx, OBJECT_ROOT_PATH, 1));
    if (PARSE_OK != NodeFromJSONPath(jt->root, spath, &jpn)) {
        ReplyWithSearchPathError(ctx, jpn);
        goto error;
    }

    // deal with path errors
    if (E_OK != jpn->err) {
        ReplyWithPathError(ctx, jpn);
        goto error;
    }

    // verify that the target's type is an array
    if (N_ARRAY != NODETYPE(jpn->n)) {
        ReplyWithPathTypeError(ctx, N_ARRAY, NODETYPE(jpn->n));
        goto error;
    }

    // nothing to do
    long long len = Node_Length(jpn->n);
    if (!len) {
        RedisModule_ReplyWithNull(ctx);
        goto ok;
    }

    // get the index
    long long index = -1;
    if (argc > 3 && REDISMODULE_OK != RedisModule_StringToLongLong(argv[3], &index)) {
        RedisModule_ReplyWithError(ctx, REJSON_ERROR_INDEX_INVALID);
        goto error;
    }

    // convert negative index
    if (index < 0) index = len + index;
    if (index < 0) index = 0;
    if (index >= len) index = len - 1;

    // get and serialize the popped array item
    JSONSerializeOpt jsopt = {0};
    sds json = sdsempty();
    Node *item;
    Node_ArrayItem(jpn->n, index, &item);
    SerializeNodeToJSON(item, &jsopt, &json);

    // check whether serialization had succeeded
    if (!sdslen(json)) {
        sdsfree(json);
        RM_LOG_WARNING(ctx, "%s", REJSON_ERROR_SERIALIZE);
        RedisModule_ReplyWithError(ctx, REJSON_ERROR_SERIALIZE);
        goto error;
    }

    // delete the item from the array
    Node_ArrayDelRange(jpn->n, index, 1);
    maybeClearPathCache(jt, jpn);

    // reply with the serialization
    RedisModule_ReplyWithStringBuffer(ctx, json, sdslen(json));
    sdsfree(json);

ok:
    JSONPathNode_Free(jpn);
    RedisModule_ReplicateVerbatim(ctx);
    return REDISMODULE_OK;

error:
    JSONPathNode_Free(jpn);
    return REDISMODULE_ERR;
}

/**
 * JSON.ARRTRIM <key> <path> <start> <stop>
 * Trim an array so that it contains only the specified inclusive range of elements.
 *
 * This command is extremely forgiving and using it with out of range indexes will not produce
 * an error. If `start` is larger than the array's size or `start` > `stop`, the result will be
 * an empty array. If `start` is < 0 then it will be treated as 0. If end is larger than the end
 * of the array, it will be treated like the last element in it.
 *
 * Reply: Integer, specifically the array's new size.
 */
int JSONArrTrim_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    // check args
    if (argc != 5) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }
    RedisModule_AutoMemory(ctx);

    // key can't be empty and must be a JSON type
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    int type = RedisModule_KeyType(key);
    if (REDISMODULE_KEYTYPE_EMPTY == type || RedisModule_ModuleTypeGetType(key) != JSONType) {
        RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
        return REDISMODULE_ERR;
    }

    // validate path
    JSONType_t *jt = RedisModule_ModuleTypeGetValue(key);
    JSONPathNode_t *jpn = NULL;
    if (PARSE_OK != NodeFromJSONPath(jt->root, argv[2], &jpn)) {
        ReplyWithSearchPathError(ctx, jpn);
        goto error;
    }

    // deal with path errors
    if (E_OK != jpn->err) {
        ReplyWithPathError(ctx, jpn);
        goto error;
    }

    // verify that the target's type is an array
    if (N_ARRAY != NODETYPE(jpn->n)) {
        ReplyWithPathTypeError(ctx, N_ARRAY, NODETYPE(jpn->n));
        goto error;
    }

    // get start & stop
    long long start, stop, left, right;
    long long len = (long long)Node_Length(jpn->n);
    if (REDISMODULE_OK != RedisModule_StringToLongLong(argv[3], &start)) {
        RedisModule_ReplyWithError(ctx, REJSON_ERROR_INDEX_INVALID);
        goto error;
    }
    if (REDISMODULE_OK != RedisModule_StringToLongLong(argv[4], &stop)) {
        RedisModule_ReplyWithError(ctx, REJSON_ERROR_INDEX_INVALID);
        goto error;
    }

    // convert negative indexes
    if (start < 0) start = len + start;
    if (stop < 0) stop = len + stop;

    if (start < 0) start = 0;            // start at the beginning
    if (start > stop || start >= len) {  // empty the array
        left = len;
        right = 0;
    } else {  // set the boundries
        left = start;
        if (stop >= len) stop = len - 1;
        right = len - stop - 1;
    }

    // trim the array
    Node_ArrayDelRange(jpn->n, 0, left);
    Node_ArrayDelRange(jpn->n, -right, right);

    RedisModule_ReplyWithLongLong(ctx, (long long)Node_Length(jpn->n));
    maybeClearPathCache(jt, jpn);
    JSONPathNode_Free(jpn);
    RedisModule_ReplicateVerbatim(ctx);
    return REDISMODULE_OK;

error:
    JSONPathNode_Free(jpn);
    return REDISMODULE_ERR;
}

int JSONCacheInfoCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    // Just dump and return the Cache Info
    RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);
    size_t numElems = 0;

    RedisModule_ReplyWithSimpleString(ctx, "bytes");
    RedisModule_ReplyWithLongLong(ctx, REJSON_LRUCACHE_GLOBAL->numBytes);
    numElems += 2;

    RedisModule_ReplyWithSimpleString(ctx, "items");
    RedisModule_ReplyWithLongLong(ctx, REJSON_LRUCACHE_GLOBAL->numEntries);
    numElems += 2;

    RedisModule_ReplyWithSimpleString(ctx, "max_bytes");
    RedisModule_ReplyWithLongLong(ctx, REJSON_LRUCACHE_GLOBAL->maxBytes);
    numElems += 2;

    RedisModule_ReplyWithSimpleString(ctx, "max_entries");
    RedisModule_ReplyWithLongLong(ctx, REJSON_LRUCACHE_GLOBAL->maxEntries);
    numElems += 2;

    RedisModule_ReplyWithSimpleString(ctx, "min_size");
    RedisModule_ReplyWithLongLong(ctx, REJSON_LRUCACHE_GLOBAL->minSize);

    numElems += 2;
    RedisModule_ReplySetArrayLength(ctx, numElems);
    return REDISMODULE_OK;
}

int JSONCacheInitCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    // Initialize the cache settings. This is temporary and used for tests
    long long maxByte = LRUCACHE_DEFAULT_MAXBYTE, maxEnt = LRUCACHE_DEFAULT_MAXENT,
              minSize = LRUCACHE_DEFAULT_MINSIZE;
    if (argc == 4) {
        if (RMUtil_ParseArgs(argv, argc, 1, "lll", &maxByte, &maxEnt, &minSize) != REDISMODULE_OK) {
            return RedisModule_ReplyWithError(ctx, "Bad arguments");
        }
    } else if (argc != 1) {
        return RedisModule_ReplyWithError(ctx, "USAGE: [MAXBYTES, MAXENTS, MINSIZE]");
    }

    REJSON_LRUCACHE_GLOBAL->maxBytes = maxByte;
    REJSON_LRUCACHE_GLOBAL->maxEntries = maxEnt;
    REJSON_LRUCACHE_GLOBAL->minSize = minSize;
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* Creates the module's commands. */
int Module_CreateCommands(RedisModuleCtx *ctx) {
    /* Generic JSON type commands. */
    if (RedisModule_CreateCommand(ctx, "json.resp", JSONResp_RedisCommand, "readonly", 1, 1, 1) ==
        REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "json.debug", JSONDebug_RedisCommand, "readonly getkeys-api",
                                  2, 2, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "json.type", JSONType_RedisCommand, "readonly", 1, 1, 1) ==
        REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "json.set", JSONSet_RedisCommand, "write deny-oom", 1, 1,
                                  1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "json.get", JSONGet_RedisCommand, "readonly", 1, 1, 1) ==
        REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "json.mget", JSONMGet_RedisCommand, "readonly getkeys-api",
                                  1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "json.del", JSONDel_RedisCommand, "write", 1, 1, 1) ==
        REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "json.forget", JSONDel_RedisCommand, "write", 1, 1, 1) ==
        REDISMODULE_ERR)
        return REDISMODULE_ERR;

    /* JSON number commands. */
    if (RedisModule_CreateCommand(ctx, "json.numincrby", JSONNum_GenericCommand, "write", 1, 1,
                                  1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "json.nummultby", JSONNum_GenericCommand, "write", 1, 1,
                                  1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    /* JSON string commands. */
    if (RedisModule_CreateCommand(ctx, "json.strlen", JSONLen_GenericCommand, "readonly", 1, 1,
                                  1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "json.strappend", JSONStrAppend_RedisCommand,
                                  "write deny-oom", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    /* JSON array commands matey. */
    if (RedisModule_CreateCommand(ctx, "json.arrlen", JSONLen_GenericCommand, "readonly", 1, 1,
                                  1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "json.arrinsert", JSONArrInsert_RedisCommand,
                                  "write deny-oom", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "json.arrappend", JSONArrAppend_RedisCommand,
                                  "write deny-oom", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "json.arrindex", JSONArrIndex_RedisCommand, "readonly", 1, 1,
                                  1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "json.arrpop", JSONArrPop_RedisCommand, "write", 1, 1, 1) ==
        REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "json.arrtrim", JSONArrTrim_RedisCommand, "write", 1, 1,
                                  1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    /* JSON object commands. */
    if (RedisModule_CreateCommand(ctx, "json.objlen", JSONLen_GenericCommand, "readonly", 1, 1,
                                  1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "json.objkeys", JSONObjKeys_RedisCommand, "readonly", 1, 1,
                                  1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "json._cacheinfo", JSONCacheInfoCommand, "readonly", 1, 1,
                                  1) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    if (RedisModule_CreateCommand(ctx, "json._cacheinit", JSONCacheInitCommand, "write", 1, 1, 1) ==
        REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    // Register the module
    if (RedisModule_Init(ctx, RLMODULE_NAME, REJSON_MODULE_VERSION, REDISMODULE_APIVER_1) ==
        REDISMODULE_ERR)
        return REDISMODULE_ERR;

    // Scan the arguments
    for (size_t ii = 0; ii < argc; ++ii) {
        const char *s = RedisModule_StringPtrLen(argv[ii], NULL);
        if (!strcasecmp(s, "CACHE")) {
            const char *v = RedisModule_StringPtrLen(argv[++ii], NULL);
            if (!strcasecmp(v, "ON") || !strcasecmp(v, "TRUE")) {
                RedisModule_Log(ctx, "notice", "LRU Caching enabled");
                jsonLruCacheEnabled_g = 1;
            } else if (!strcasecmp(v, "OFF") || !strcasecmp(v, "FALSE")) {
                RedisModule_Log(ctx, "notice", "LRU Caching disabled");
                jsonLruCacheEnabled_g = 0;
            } else {
                RedisModule_Log(ctx, "warning", "CACHE requires a value of ON|OFF");
                return REDISMODULE_ERR;
            }
        } else {
            RedisModule_Log(ctx, "warning", "Unknown option %s", s);
            return REDISMODULE_ERR;
        }
    }

    // Register the JSON data type
    RedisModuleTypeMethods tm = {.version = REDISMODULE_TYPE_METHOD_VERSION,
                                 .rdb_load = JSONTypeRdbLoad,
                                 .rdb_save = JSONTypeRdbSave,
                                 .aof_rewrite = JSONTypeAofRewrite,
                                 .mem_usage = JSONTypeMemoryUsage,
                                 .free = JSONTypeFree};
    JSONType = RedisModule_CreateDataType(ctx, JSONTYPE_NAME, JSONTYPE_ENCODING_VERSION, &tm);
    if (NULL == JSONType) return REDISMODULE_ERR;

    // Initialize the module's context
    JSONCtx = (ModuleCtx){0};
    JSONCtx.joctx = NewJSONObjectCtx(0);

    // Create the commands
    if (REDISMODULE_ERR == Module_CreateCommands(ctx)) return REDISMODULE_ERR;

    RM_LOG_WARNING(ctx, "%s v%d.%d.%d [encver %d]", RLMODULE_DESC, REJSON_VERSION_MAJOR,
                   REJSON_VERSION_MINOR, REJSON_VERSION_PATCH, JSONTYPE_ENCODING_VERSION);

    return REDISMODULE_OK;
}
