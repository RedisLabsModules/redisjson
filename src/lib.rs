#[macro_use]
extern crate redis_module;

use redis_module::native_types::RedisType;
use redis_module::raw::RedisModuleTypeMethods;
use redis_module::{raw as rawmod, NextArg};
use redis_module::{Context, RedisError, RedisResult, RedisValue, REDIS_OK};
use serde_json::{Number, Value};

use std::{i64, usize};

mod array_index;
mod backward;
mod error;
mod formatter;
mod nodevisitor;
mod redisjson;

use crate::array_index::ArrayIndex;
use crate::error::Error;
use crate::redisjson::{Format, Path, RedisJSON, SetOptions};

pub const REDIS_JSON_TYPE_VERSION: i32 = 2;

static REDIS_JSON_TYPE: RedisType = RedisType::new(
    "ReJSON-RL",
    REDIS_JSON_TYPE_VERSION,
    RedisModuleTypeMethods {
        version: redis_module::TYPE_METHOD_VERSION,

        rdb_load: Some(redisjson::type_methods::rdb_load),
        rdb_save: Some(redisjson::type_methods::rdb_save),
        aof_rewrite: Some(redisjson::type_methods::aof_rewrite),
        free: Some(redisjson::type_methods::free),

        // Currently unused by Redis
        mem_usage: None,
        digest: None,

        // Auxiliary data (v2)
        aux_load: None,
        aux_save: None,
        aux_save_triggers: rawmod::Aux::Before as i32,
    },
);

///
/// Backwards compatibility convertor for RedisJSON 1.x clients
///
fn backwards_compat_path(mut path: String) -> String {
    if !path.starts_with('$') {
        if path == "." {
            path.replace_range(..1, "$");
        } else if path.starts_with('.') {
            path.insert(0, '$');
        } else {
            path.insert_str(0, "$.");
        }
    }
    path
}

///
/// JSON.DEL <key> [path]
///
fn json_del(ctx: &Context, args: Vec<String>) -> RedisResult {
    let mut args = args.into_iter().skip(1);

    let key = args.next_string()?;
    let path = backwards_compat_path(args.next_string()?);

    let key = ctx.open_key_writable(&key);
    let deleted = match key.get_value::<RedisJSON>(&REDIS_JSON_TYPE)? {
        Some(doc) => {
            let res = if path == "$" {
                key.delete()?;
                1
            } else {
                doc.delete_path(&path)?
            };
            ctx.replicate_verbatim();
            res
        }
        None => 0,
    };
    Ok(deleted.into())
}

///
/// JSON.SET <key> <path> <json> [NX | XX | FORMAT <format>]
///
fn json_set(ctx: &Context, args: Vec<String>) -> RedisResult {
    let mut args = args.into_iter().skip(1);

    let key = args.next_string()?;
    let path = backwards_compat_path(args.next_string()?);
    let value = args.next_string()?;

    let mut format = Format::JSON;
    let mut set_option = SetOptions::None;

    while let Some(s) = args.next() {
        match s.to_uppercase().as_str() {
            "NX" => set_option = SetOptions::NotExists,
            "XX" => set_option = SetOptions::AlreadyExists,
            "FORMAT" => {
                format = Format::from_str(args.next_string()?.as_str())?;
            }
            _ => break,
        };
    }

    let redis_key = ctx.open_key_writable(&key);
    let current = redis_key.get_value::<RedisJSON>(&REDIS_JSON_TYPE)?;

    match (current, set_option) {
        (Some(ref mut doc), ref op) => {
            if doc.set_value(&value, &path, op, format)? {
                ctx.replicate_verbatim();
                REDIS_OK
            } else {
                Ok(RedisValue::Null)
            }
        }
        (None, SetOptions::AlreadyExists) => Ok(RedisValue::Null),
        (None, _) => {
            let doc = RedisJSON::from_str(&value, format)?;
            if path == "$" {
                redis_key.set_value(&REDIS_JSON_TYPE, doc)?;
                ctx.replicate_verbatim();
                REDIS_OK
            } else {
                Err(RedisError::Str(
                    "ERR new objects must be created at the root",
                ))
            }
        }
    }
}

///
/// JSON.GET <key>
///         [INDENT indentation-string]
///         [NEWLINE line-break-string]
///         [SPACE space-string]
///         [path ...]
///
/// TODO add support for multi path
fn json_get(ctx: &Context, args: Vec<String>) -> RedisResult {
    let mut args = args.into_iter().skip(1);
    let key = args.next_string()?;

    let mut paths: Vec<Path> = vec![];
    let mut format = Format::JSON;
    let mut indent = String::new();
    let mut space = String::new();
    let mut newline = String::new();
    while let Ok(arg) = args.next_string() {
        match arg.to_uppercase().as_str() {
            "INDENT" => {
                indent = args.next_string()?;
            }
            "NEWLINE" => {
                newline = args.next_string()?;
            }
            "SPACE" => {
                space = args.next_string()?;
            }
            "NOESCAPE" => {
                // Silently ignore. Compatibility with ReJSON v1.0 which has this option. See #168
                continue;
            } // TODO add support
            "FORMAT" => {
                format = Format::from_str(args.next_string()?.as_str())?;
            }
            _ => {
                paths.push(Path::new(arg));
            }
        };
    }

    // path is optional -> no path found we use root "$"
    if paths.is_empty() {
        paths.push(Path::new("$".to_string()));
    }

    let key = ctx.open_key_writable(&key);
    let value = match key.get_value::<RedisJSON>(&REDIS_JSON_TYPE)? {
        Some(doc) => doc
            .to_json(&mut paths, indent, newline, space, format)?
            .into(),
        None => RedisValue::Null,
    };

    Ok(value)
}

///
/// JSON.MGET <key> [key ...] <path>
///
fn json_mget(ctx: &Context, args: Vec<String>) -> RedisResult {
    if args.len() < 3 {
        return Err(RedisError::WrongArity);
    }

    args.last().ok_or(RedisError::WrongArity).and_then(|path| {
        let path = backwards_compat_path(path.to_string());
        let keys = &args[1..args.len() - 1];

        let results: Result<Vec<RedisValue>, RedisError> = keys
            .iter()
            .map(|key| {
                let result = ctx
                    .open_key(key)
                    .get_value::<RedisJSON>(&REDIS_JSON_TYPE)?
                    .map(|doc| doc.to_string(&path, Format::JSON))
                    .transpose()?;

                Ok(result.into())
            })
            .collect();

        Ok(results?.into())
    })
}

///
/// JSON.STRLEN <key> [path]
///
fn json_str_len(ctx: &Context, args: Vec<String>) -> RedisResult {
    json_len(ctx, args, |doc, path| doc.str_len(path))
}

///
/// JSON.TYPE <key> [path]
///
fn json_type(ctx: &Context, args: Vec<String>) -> RedisResult {
    let mut args = args.into_iter().skip(1);
    let key = args.next_string()?;
    let path = backwards_compat_path(args.next_string()?);

    let key = ctx.open_key(&key);

    let value = key.get_value::<RedisJSON>(&REDIS_JSON_TYPE)?.map_or_else(
        || RedisValue::Null,
        |doc| match doc.get_type(&path) {
            Ok(s) => s.into(),
            Err(_) => RedisValue::Null,
        },
    );

    Ok(value)
}

///
/// JSON.NUMINCRBY <key> <path> <number>
///
fn json_num_incrby(ctx: &Context, args: Vec<String>) -> RedisResult {
    json_num_op(ctx, args, |i1, i2| i1 + i2, |f1, f2| f1 + f2)
}

///
/// JSON.NUMMULTBY <key> <path> <number>
///
fn json_num_multby(ctx: &Context, args: Vec<String>) -> RedisResult {
    json_num_op(ctx, args, |i1, i2| i1 * i2, |f1, f2| f1 * f2)
}

///
/// JSON.NUMPOWBY <key> <path> <number>
///
fn json_num_powby(ctx: &Context, args: Vec<String>) -> RedisResult {
    json_num_op(ctx, args, |i1, i2| i1.pow(i2 as u32), |f1, f2| f1.powf(f2))
}

fn json_num_op<I, F>(ctx: &Context, args: Vec<String>, op_i64: I, op_f64: F) -> RedisResult
where
    I: Fn(i64, i64) -> i64,
    F: Fn(f64, f64) -> f64,
{
    let mut args = args.into_iter().skip(1);

    let key = args.next_string()?;
    let path = backwards_compat_path(args.next_string()?);
    let number = args.next_string()?;

    let key = ctx.open_key_writable(&key);

    key.get_value::<RedisJSON>(&REDIS_JSON_TYPE)?
        .ok_or_else(RedisError::nonexistent_key)
        .and_then(|doc| {
            doc.value_op(&path, |value| {
                do_json_num_op(&number, value, &op_i64, &op_f64)
            })
            .map(|v| {
                ctx.replicate_verbatim();
                v.to_string().into()
            })
            .map_err(|e| e.into())
        })
}

fn do_json_num_op<I, F>(
    in_value: &str,
    curr_value: &Value,
    op_i64: I,
    op_f64: F,
) -> Result<Value, Error>
where
    I: FnOnce(i64, i64) -> i64,
    F: FnOnce(f64, f64) -> f64,
{
    if let Value::Number(curr_value) = curr_value {
        let in_value = &serde_json::from_str(in_value)?;
        if let Value::Number(in_value) = in_value {
            let num_res = match (curr_value.as_i64(), in_value.as_i64()) {
                (Some(num1), Some(num2)) => op_i64(num1, num2).into(),
                _ => {
                    let num1 = curr_value.as_f64().unwrap();
                    let num2 = in_value.as_f64().unwrap();
                    Number::from_f64(op_f64(num1, num2)).unwrap()
                }
            };

            Ok(Value::Number(num_res))
        } else {
            Err(err_json(in_value, "number"))
        }
    } else {
        Err(err_json(curr_value, "number"))
    }
}

fn err_json(value: &Value, expected_value: &'static str) -> Error {
    Error::from(format!(
        "ERR wrong type of path value - expected {} but found {}",
        expected_value,
        RedisJSON::value_name(value)
    ))
}

///
/// JSON.STRAPPEND <key> [path] <json-string>
///
fn json_str_append(ctx: &Context, args: Vec<String>) -> RedisResult {
    let mut args = args.into_iter().skip(1);

    let key = args.next_string()?;
    let path_or_json = args.next_string()?;

    let path;
    let json;

    // path is optional
    if let Ok(val) = args.next_string() {
        path = backwards_compat_path(path_or_json);
        json = val;
    } else {
        path = "$".to_string();
        json = path_or_json;
    }

    let key = ctx.open_key_writable(&key);

    key.get_value::<RedisJSON>(&REDIS_JSON_TYPE)?
        .ok_or_else(RedisError::nonexistent_key)
        .and_then(|doc| {
            doc.value_op(&path, |value| do_json_str_append(&json, value))
                .map(|v| {
                    ctx.replicate_verbatim();
                    v.as_str().map_or(usize::MAX, |v| v.len()).into()
                })
                .map_err(|e| e.into())
        })
}

fn do_json_str_append(json: &str, value: &Value) -> Result<Value, Error> {
    value
        .as_str()
        .ok_or_else(|| err_json(value, "string"))
        .and_then(|curr| {
            let v = serde_json::from_str(json)?;
            if let Value::String(s) = v {
                let new_value = [curr, s.as_str()].concat();
                Ok(Value::String(new_value))
            } else {
                Err(format!("ERR wrong type of value - expected string but found {}", v).into())
            }
        })
}

///
/// JSON.ARRAPPEND <key> <path> <json> [json ...]
///
fn json_arr_append(ctx: &Context, args: Vec<String>) -> RedisResult {
    let mut args = args.into_iter().skip(1).peekable();

    let key = args.next_string()?;
    let path = backwards_compat_path(args.next_string()?);

    // We require at least one JSON item to append
    args.peek().ok_or(RedisError::WrongArity)?;

    let key = ctx.open_key_writable(&key);

    key.get_value::<RedisJSON>(&REDIS_JSON_TYPE)?
        .ok_or_else(RedisError::nonexistent_key)
        .and_then(|doc| {
            doc.value_op(&path, |value| do_json_arr_append(args.clone(), value))
                .map(|v| {
                    ctx.replicate_verbatim();
                    v.as_array().map_or(usize::MAX, |v| v.len()).into()
                })
                .map_err(|e| e.into())
        })
}

fn do_json_arr_append<I>(args: I, value: &Value) -> Result<Value, Error>
where
    I: Iterator<Item = String>,
{
    value
        .as_array()
        .ok_or_else(|| err_json(value, "array"))
        .and_then(|curr| {
            let items: Vec<Value> = args
                .map(|json| serde_json::from_str(&json))
                .collect::<Result<_, _>>()?;

            let new_value = [curr.as_slice(), &items].concat();
            Ok(Value::Array(new_value))
        })
}

///
/// JSON.ARRINDEX <key> <path> <json-scalar> [start [stop]]
///
/// scalar - number, string, Boolean (true or false), or null
///
fn json_arr_index(ctx: &Context, args: Vec<String>) -> RedisResult {
    let mut args = args.into_iter().skip(1);

    let key = args.next_string()?;
    let path = backwards_compat_path(args.next_string()?);
    let json_scalar = args.next_string()?;
    let start: i64 = args.next().map(|v| v.parse()).unwrap_or(Ok(0))?;
    let end: i64 = args.next().map(|v| v.parse()).unwrap_or(Ok(i64::MAX))?;

    args.done()?; // TODO: Add to other functions as well to terminate args list

    let key = ctx.open_key(&key);

    let index = key
        .get_value::<RedisJSON>(&REDIS_JSON_TYPE)?
        .map_or(Ok(-1), |doc| doc.arr_index(&path, &json_scalar, start, end))?;

    Ok(index.into())
}

///
/// JSON.ARRINSERT <key> <path> <index> <json> [json ...]
///
fn json_arr_insert(ctx: &Context, args: Vec<String>) -> RedisResult {
    let mut args = args.into_iter().skip(1).peekable();

    let key = args.next_string()?;
    let path = backwards_compat_path(args.next_string()?);
    let index = args.next_i64()?;

    // We require at least one JSON item to append
    args.peek().ok_or(RedisError::WrongArity)?;

    let key = ctx.open_key_writable(&key);

    key.get_value::<RedisJSON>(&REDIS_JSON_TYPE)?
        .ok_or_else(RedisError::nonexistent_key)
        .and_then(|doc| {
            doc.value_op(&path, |value| {
                do_json_arr_insert(args.clone(), index, value)
            })
            .map(|v| {
                ctx.replicate_verbatim();
                v.as_array().map_or(usize::MAX, |v| v.len()).into()
            })
            .map_err(|e| e.into())
        })
}

fn do_json_arr_insert<I>(args: I, index: i64, value: &Value) -> Result<Value, Error>
where
    I: Iterator<Item = String>,
{
    value
        .as_array()
        .ok_or_else(|| err_json(value, "array"))
        .and_then(|curr| {
            let len = curr.len() as i64;

            if !(-len..len).contains(&index) {
                return Err("ERR index out of bounds".into());
            }

            let index = index.normalize(len);

            let items: Vec<Value> = args
                .map(|json| serde_json::from_str(&json))
                .collect::<Result<_, _>>()?;

            let mut new_value = curr.to_owned();
            new_value.splice(index..index, items.into_iter());

            Ok(Value::Array(new_value))
        })
}

///
/// JSON.ARRLEN <key> [path]
///
fn json_arr_len(ctx: &Context, args: Vec<String>) -> RedisResult {
    json_len(ctx, args, |doc, path| doc.arr_len(path))
}

///
/// JSON.ARRPOP <key> [path [index]]
///
fn json_arr_pop(ctx: &Context, args: Vec<String>) -> RedisResult {
    let mut args = args.into_iter().skip(1);

    let key = args.next_string()?;

    let (path, index) = args
        .next()
        .map(|p| {
            let path = backwards_compat_path(p);
            let index = args.next_i64().unwrap_or(i64::MAX);
            (path, index)
        })
        .unwrap_or(("$".to_string(), i64::MAX));

    let key = ctx.open_key_writable(&key);
    let mut res = Value::Null;

    key.get_value::<RedisJSON>(&REDIS_JSON_TYPE)?
        .ok_or_else(RedisError::nonexistent_key)
        .and_then(|doc| {
            doc.value_op(&path, |value| do_json_arr_pop(index, &mut res, value))
                .map(|v| {
                    ctx.replicate_verbatim();
                    v
                })
                .map_err(|e| e.into())
        })?;
    Ok(RedisJSON::serialize(&res, Format::JSON)?.into())
}

fn do_json_arr_pop(mut index: i64, res: &mut Value, value: &Value) -> Result<Value, Error> {
    value
        .as_array()
        .ok_or_else(|| err_json(value, "array"))
        .and_then(|curr| {
            let len = curr.len() as i64;

            index = index.min(len - 1);

            if index < 0 {
                index += len;
            }

            if index >= len || index < 0 {
                return Err("ERR index out of bounds".into());
            }

            let mut new_value = curr.to_owned();
            *res = new_value.remove(index as usize);
            Ok(Value::Array(new_value))
        })
}

///
/// JSON.ARRTRIM <key> <path> <start> <stop>
///
fn json_arr_trim(ctx: &Context, args: Vec<String>) -> RedisResult {
    let mut args = args.into_iter().skip(1);

    let key = args.next_string()?;
    let path = backwards_compat_path(args.next_string()?);
    let start = args.next_i64()?;
    let stop = args.next_i64()?;

    let key = ctx.open_key_writable(&key);

    key.get_value::<RedisJSON>(&REDIS_JSON_TYPE)?
        .ok_or_else(RedisError::nonexistent_key)
        .and_then(|doc| {
            doc.value_op(&path, |value| do_json_arr_trim(start, stop, &value))
                .map(|v| {
                    ctx.replicate_verbatim();
                    v.as_array().map_or(usize::MAX, |v| v.len()).into()
                })
                .map_err(|e| e.into())
        })
}

fn do_json_arr_trim(start: i64, stop: i64, value: &Value) -> Result<Value, Error> {
    value
        .as_array()
        .ok_or_else(|| err_json(value, "array"))
        .and_then(|curr| {
            let len = curr.len() as i64;
            let stop = stop.normalize(len);

            let range = if start > len || start > stop as i64 {
                0..0 // Return an empty array
            } else {
                start.normalize(len)..(stop + 1)
            };

            let res = &curr[range];
            Ok(Value::Array(res.to_vec()))
        })
}

///
/// JSON.OBJKEYS <key> [path]
///
fn json_obj_keys(ctx: &Context, args: Vec<String>) -> RedisResult {
    let mut args = args.into_iter().skip(1);
    let key = args.next_string()?;
    let path = backwards_compat_path(args.next_string()?);

    let key = ctx.open_key(&key);

    let value = match key.get_value::<RedisJSON>(&REDIS_JSON_TYPE)? {
        Some(doc) => doc.obj_keys(&path)?.into(),
        None => RedisValue::Null,
    };

    Ok(value)
}

///
/// JSON.OBJLEN <key> [path]
///
fn json_obj_len(ctx: &Context, args: Vec<String>) -> RedisResult {
    json_len(ctx, args, |doc, path| doc.obj_len(path))
}

///
/// JSON.DEBUG <subcommand & arguments>
///
/// subcommands:
/// MEMORY <key> [path]
/// HELP
///
fn json_debug(ctx: &Context, args: Vec<String>) -> RedisResult {
    let mut args = args.into_iter().skip(1);
    match args.next_string()?.to_uppercase().as_str() {
        "MEMORY" => {
            let key = args.next_string()?;
            let path = backwards_compat_path(args.next_string()?);

            let key = ctx.open_key(&key);
            let value = match key.get_value::<RedisJSON>(&REDIS_JSON_TYPE)? {
                Some(doc) => doc.get_memory(&path)?,
                None => 0,
            };
            Ok(value.into())
        }
        "HELP" => {
            let results = vec![
                "MEMORY <key> [path] - reports memory usage",
                "HELP                - this message",
            ];
            Ok(results.into())
        }
        _ => Err(RedisError::Str(
            "ERR unknown subcommand - try `JSON.DEBUG HELP`",
        )),
    }
}

///
/// JSON.RESP <key> [path]
///
fn json_resp(ctx: &Context, args: Vec<String>) -> RedisResult {
    let mut args = args.into_iter().skip(1);

    let key = args.next_string()?;
    let path = backwards_compat_path(args.next_string()?);

    let key = ctx.open_key(&key);
    match key.get_value::<RedisJSON>(&REDIS_JSON_TYPE)? {
        Some(doc) => Ok(resp_serialize(doc.get_first(&path)?)),
        None => Ok(RedisValue::Null),
    }
}

fn resp_serialize(doc: &Value) -> RedisValue {
    match doc {
        Value::Null => RedisValue::Null,

        Value::Bool(b) => RedisValue::SimpleString(b.to_string()),

        Value::Number(n) => n
            .as_i64()
            .map(RedisValue::Integer)
            .unwrap_or_else(|| RedisValue::Float(n.as_f64().unwrap())),

        Value::String(s) => RedisValue::BulkString(s.clone()),

        Value::Array(arr) => {
            let mut res: Vec<RedisValue> = Vec::with_capacity(arr.len() + 1);
            res.push(RedisValue::SimpleStringStatic("["));
            arr.iter().for_each(|v| res.push(resp_serialize(v)));
            RedisValue::Array(res)
        }

        Value::Object(obj) => {
            let mut res: Vec<RedisValue> = Vec::with_capacity(obj.len() + 1);
            res.push(RedisValue::SimpleStringStatic("{"));
            for (key, value) in obj.iter() {
                res.push(RedisValue::BulkString(key.to_string()));
                res.push(resp_serialize(value));
            }
            RedisValue::Array(res)
        }
    }
}

fn json_len<F: Fn(&RedisJSON, &String) -> Result<usize, Error>>(
    ctx: &Context,
    args: Vec<String>,
    fun: F,
) -> RedisResult {
    let mut args = args.into_iter().skip(1);
    let key = args.next_string()?;
    let path = backwards_compat_path(args.next_string()?);

    let key = ctx.open_key(&key);
    let length = match key.get_value::<RedisJSON>(&REDIS_JSON_TYPE)? {
        Some(doc) => fun(&doc, &path)?.into(),
        None => RedisValue::Null,
    };

    Ok(length)
}

fn json_cache_info(_ctx: &Context, _args: Vec<String>) -> RedisResult {
    Err(RedisError::Str("Command was not implemented"))
}

fn json_cache_init(_ctx: &Context, _args: Vec<String>) -> RedisResult {
    Err(RedisError::Str("Command was not implemented"))
}
//////////////////////////////////////////////////////

redis_module! {
    name: "ReJSON",
    version: 99_99_99,
    data_types: [
        REDIS_JSON_TYPE,
    ],
    commands: [
        ["json.del", json_del, "write", 1,1,1],
        ["json.get", json_get, "readonly", 1,1,1],
        ["json.mget", json_mget, "readonly", 1,1,1],
        ["json.set", json_set, "write deny-oom", 1,1,1],
        ["json.type", json_type, "readonly", 1,1,1],
        ["json.numincrby", json_num_incrby, "write", 1,1,1],
        ["json.nummultby", json_num_multby, "write", 1,1,1],
        ["json.numpowby", json_num_powby, "write", 1,1,1],
        ["json.strappend", json_str_append, "write deny-oom", 1,1,1],
        ["json.strlen", json_str_len, "readonly", 1,1,1],
        ["json.arrappend", json_arr_append, "write deny-oom", 1,1,1],
        ["json.arrindex", json_arr_index, "readonly", 1,1,1],
        ["json.arrinsert", json_arr_insert, "write deny-oom", 1,1,1],
        ["json.arrlen", json_arr_len, "readonly", 1,1,1],
        ["json.arrpop", json_arr_pop, "write", 1,1,1],
        ["json.arrtrim", json_arr_trim, "write", 1,1,1],
        ["json.objkeys", json_obj_keys, "readonly", 1,1,1],
        ["json.objlen", json_obj_len, "readonly", 1,1,1],
        ["json.debug", json_debug, "readonly", 1,1,1],
        ["json.forget", json_del, "write", 1,1,1],
        ["json.resp", json_resp, "readonly", 1,1,1],
        ["json._cacheinfo", json_cache_info, "readonly", 1,1,1],
        ["json._cacheinit", json_cache_init, "write", 1,1,1],
    ],
}
