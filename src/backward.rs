use std::vec::Vec;

use redis_module::raw;
use serde_json::map::Map;
use serde_json::Number;
use serde_json::Value;

use crate::error::Error;
use crate::redisjson::type_methods::is_io_error;

#[derive(Debug, PartialEq)]
enum NodeType {
    Null,
    // used in masks and consistent type checking
    String,
    Number,
    Integer,
    Boolean,
    Dict,
    Array,
    KeyVal,
    // N_DATETIME = 0x100
    // N_BINARY = 0x200
}

impl From<u64> for NodeType {
    fn from(n: u64) -> Self {
        match n {
            0x1u64 => NodeType::Null,
            0x2u64 => NodeType::String,
            0x4u64 => NodeType::Number,
            0x8u64 => NodeType::Integer,
            0x10u64 => NodeType::Boolean,
            0x20u64 => NodeType::Dict,
            0x40u64 => NodeType::Array,
            0x80u64 => NodeType::KeyVal,
            _ => panic!("Can't load old RedisJSON RDB1"),
        }
    }
}

pub fn json_rdb_load(rdb: *mut raw::RedisModuleIO) -> Result<Value, Error> {
    let node_type = raw::load_unsigned(rdb).into();
    if is_io_error(rdb) {
        return Err(Error::from("short read"));
    }
    match node_type {
        NodeType::Null => Ok(Value::Null),
        NodeType::Boolean => {
            let buffer = raw::load_string_buffer(rdb);
            if is_io_error(rdb) {
                return Err(Error::from("short read"));
            }
            Ok(Value::Bool(buffer.as_ref()[0] == b'1'))
        }
        NodeType::Integer => {
            let n = raw::load_signed(rdb);
            if is_io_error(rdb) {
                return Err(Error::from("short read"));
            }
            Ok(Value::Number(n.into()))
        }
        NodeType::Number => {
            let n = raw::load_double(rdb);
            if is_io_error(rdb) {
                return Err(Error::from("short read"));
            }
            Ok(Value::Number(Number::from_f64(n).unwrap()))
        }
        NodeType::String => {
            let buffer = raw::load_string_buffer(rdb);
            if is_io_error(rdb) {
                return Err(Error::from("short read"));
            }
            Ok(Value::String(buffer.to_string().unwrap()))
        }
        NodeType::Dict => {
            let len = raw::load_unsigned(rdb);
            if is_io_error(rdb) {
                return Err(Error::from("short read"));
            }
            let mut m = Map::with_capacity(len as usize);
            for _ in 0..len {
                let u = raw::load_unsigned(rdb);
                if is_io_error(rdb) {
                    return Err(Error::from("short read"));
                }
                let t: NodeType = u.into();
                if t != NodeType::KeyVal {
                    return Err(Error::from("Can't load old RedisJSON RDB"));
                }
                let buffer = raw::load_string_buffer(rdb);
                m.insert(buffer.to_string().unwrap(), json_rdb_load(rdb)?);
            }
            Ok(Value::Object(m))
        }
        NodeType::Array => {
            let len = raw::load_unsigned(rdb);
            let mut v = Vec::with_capacity(len as usize);
            for _ in 0..len {
                let nested = json_rdb_load(rdb)?;
                v.push(nested);
            }
            Ok(Value::Array(v))
        }
        NodeType::KeyVal => Err(Error::from("Can't load old RedisJSON RDB")),
    }
}
