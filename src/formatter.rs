// Custom serde_json formatter supporting ReJSON formatting options.
// Based on serde_json::ser::PrettyFormatter
/*
Permission is hereby granted, free of charge, to any
person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the
Software without restriction, including without
limitation the rights to use, copy, modify, merge,
publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software
is furnished to do so, subject to the following
conditions:

The above copyright notice and this permission notice
shall be included in all copies or substantial portions
of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
*/

use serde_json::ser::Formatter;
use std::io;

pub struct RedisJsonFormatter<'a> {
    current_indent: usize,
    has_value: bool,
    indent: Option<&'a str>,
    space: Option<&'a str>,
    newline: Option<&'a str>,
}

impl<'a> RedisJsonFormatter<'a> {
    pub fn new(indent: Option<&'a str>, space: Option<&'a str>, newline: Option<&'a str>) -> Self {
        RedisJsonFormatter {
            current_indent: 0,
            has_value: false,
            indent,
            space,
            newline,
        }
    }

    fn new_line<W: ?Sized>(&self, wr: &mut W) -> io::Result<()>
    where
        W: io::Write,
    {
        // Write new line if defined
        if let Some(n) = self.newline.as_ref() {
            wr.write_all(n.as_bytes())?;
        }

        // Ident the next line if defined
        if let Some(s) = self.indent.as_ref() {
            let bytes = s.as_bytes();
            for _ in 0..self.current_indent {
                wr.write_all(bytes)?;
            }
        }

        Ok(())
    }
}

impl Formatter for RedisJsonFormatter<'_> {
    fn begin_array<W: ?Sized>(&mut self, writer: &mut W) -> io::Result<()>
    where
        W: io::Write,
    {
        self.current_indent += 1;
        self.has_value = false;
        writer.write_all(b"[")
    }

    fn end_array<W: ?Sized>(&mut self, writer: &mut W) -> io::Result<()>
    where
        W: io::Write,
    {
        self.current_indent -= 1;

        if self.has_value {
            self.new_line(writer)?;
        }

        writer.write_all(b"]")
    }

    fn begin_array_value<W: ?Sized>(&mut self, writer: &mut W, first: bool) -> io::Result<()>
    where
        W: io::Write,
    {
        if !first {
            writer.write_all(b",")?;
        }
        self.new_line(writer)
    }

    fn end_array_value<W: ?Sized>(&mut self, _writer: &mut W) -> io::Result<()>
    where
        W: io::Write,
    {
        self.has_value = true;
        Ok(())
    }

    fn begin_object<W: ?Sized>(&mut self, writer: &mut W) -> io::Result<()>
    where
        W: io::Write,
    {
        self.current_indent += 1;
        self.has_value = false;
        writer.write_all(b"{")
    }

    fn end_object<W: ?Sized>(&mut self, writer: &mut W) -> io::Result<()>
    where
        W: io::Write,
    {
        self.current_indent -= 1;

        if self.has_value {
            self.new_line(writer)?;
        }

        writer.write_all(b"}")
    }

    fn begin_object_key<W: ?Sized>(&mut self, writer: &mut W, first: bool) -> io::Result<()>
    where
        W: io::Write,
    {
        if !first {
            writer.write_all(b",")?;
        }
        self.new_line(writer)
    }

    fn begin_object_value<W: ?Sized>(&mut self, writer: &mut W) -> io::Result<()>
    where
        W: io::Write,
    {
        writer.write_all(b":")?;
        if let Some(s) = self.space.as_ref() {
            writer.write_all(s.as_bytes())
        } else {
            Ok(())
        }
    }

    fn end_object_value<W: ?Sized>(&mut self, _writer: &mut W) -> io::Result<()>
    where
        W: io::Write,
    {
        self.has_value = true;
        Ok(())
    }
}
