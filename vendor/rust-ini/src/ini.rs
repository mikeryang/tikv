// The MIT License (MIT)

// Copyright (c) 2014 Y. T. CHUNG

// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
// the Software, and to permit persons to whom the Software is furnished to do so,
// subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

//! Ini

use std::char;
use std::error;

use std::fmt::{self, Display};
use std::fs::{File, OpenOptions};
use std::io::{self, Read, Write};
use std::ops::{Index, IndexMut};
use std::path::Path;
use std::str::Chars;

#[cfg(feature = "preserve_order")]
use indexmap::map::{Entry, IndexMap as Map, IntoIter, Iter, IterMut, Keys};
#[cfg(not(feature = "preserve_order"))]
use multimap::MultiMap;
#[cfg(not(feature = "preserve_order"))]
use std::collections::hash_map::{Entry, HashMap as Map, IntoIter, Iter, IterMut, Keys};

#[derive(Debug, PartialEq, Copy, Clone)]
pub enum EscapePolicy {
    /// escape absolutely nothing (dangerous)
    Nothing,
    /// only escape the most necessary things
    Basics,
    /// escape basics and non-ascii characters
    BasicsUnicode,
    /// Escape reserved symbols.
    Reserved,
    /// Escape reserved symbols and non-ascii characters
    ReservedUnicode,
    /// Escape everything that some INI implementations assume
    Everything,
}

impl EscapePolicy {
    fn escape_basics(self) -> bool {
        match self {
            EscapePolicy::Nothing => false,
            _ => true,
        }
    }

    fn escape_reserved(self) -> bool {
        match self {
            EscapePolicy::Reserved => true,
            EscapePolicy::ReservedUnicode => true,
            EscapePolicy::Everything => true,
            _ => false,
        }
    }

    fn escape_unicode(self) -> bool {
        match self {
            EscapePolicy::BasicsUnicode => true,
            EscapePolicy::ReservedUnicode => true,
            EscapePolicy::Everything => true,
            _ => false,
        }
    }

    /// Given a character this returns true if it should be escaped as
    /// per this policy or false if not.
    pub fn should_escape(self, c: char) -> bool {
        match c {
            '\\' | '\x00'..='\x1f' | '\x7f'..='\u{00ff}' => self.escape_basics(),
            ';' | '#' | '=' | ':' => self.escape_reserved(),
            '\u{0080}'..='\u{FFFF}' => self.escape_unicode(),
            _ => false,
        }
    }
}

// Escape non-INI characters
//
// Common escape sequences: https://en.wikipedia.org/wiki/INI_file#Escape_characters
//
// * `\\` \ (a single backslash, escaping the escape character)
// * `\0` Null character
// * `\a` Bell/Alert/Audible
// * `\b` Backspace, Bell character for some applications
// * `\t` Tab character
// * `\r` Carriage return
// * `\n` Line feed
// * `\;` Semicolon
// * `\#` Number sign
// * `\=` Equals sign
// * `\:` Colon
// * `\x????` Unicode character with hexadecimal code point corresponding to ????
fn escape_str(s: &str, policy: EscapePolicy) -> String {
    let mut escaped: String = String::with_capacity(s.len());
    for c in s.chars() {
        // if we know this is not something to escape as per policy, we just
        // write it and continue.
        if !policy.should_escape(c) {
            escaped.push(c);
            continue;
        }

        match c {
            '\\' => escaped.push_str("\\\\"),
            '\0' => escaped.push_str("\\0"),
            '\x01'..='\x06' | '\x0e'..='\x1f' | '\x7f'..='\u{00ff}' => {
                escaped.push_str(&format!("\\x{:04x}", c as isize)[..])
            }
            '\x07' => escaped.push_str("\\a"),
            '\x08' => escaped.push_str("\\b"),
            '\x0c' => escaped.push_str("\\f"),
            '\x0b' => escaped.push_str("\\v"),
            '\n' => escaped.push_str("\\n"),
            '\t' => escaped.push_str("\\t"),
            '\r' => escaped.push_str("\\r"),
            '\u{0080}'..='\u{FFFF}' => escaped.push_str(&format!("\\x{:04x}", c as isize)[..]),
            _ => {
                escaped.push('\\');
                escaped.push(c);
            }
        }
    }
    escaped
}

/// Parsing configuration
pub struct ParseOption {
    /// Allow quote (" or ') in value
    /// For example
    /// ```ini
    /// [Section]
    /// Key1="Quoted value"
    /// Key2='Single Quote' with extra value
    /// ```
    ///
    /// In this example, Value of `Key1` is `Quoted value`,
    /// and value of `Key2` is `Single Quote with extra value`
    /// if `enabled_quote` is set to `true`.
    pub enabled_quote: bool,

    /// Interpret `\` as an escape character
    /// For example
    /// ```ini
    /// [Section]
    /// Key1=C:\Windows
    /// ```
    ///
    /// If `enabled_escape` is true, then the value of `Key` will become `C:Windows` (`\W` equals to `W`).
    pub enabled_escape: bool,
}

impl Default for ParseOption {
    fn default() -> ParseOption {
        ParseOption { enabled_quote: true,
                      enabled_escape: true }
    }
}

/// Newline style
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum LineSeparator {
    /// System-dependent line separator
    ///
    /// On UNIX system, uses "\n"
    /// On Windows system, uses "\r\n"
    SystemDefault,

    /// Uses "\n" as new line separator
    CR,

    /// Uses "\r\n" as new line separator
    CRLF,
}

#[cfg(not(windows))]
static DEFAULT_LINE_SEPARATOR: &str = "\n";

#[cfg(windows)]
static DEFAULT_LINE_SEPARATOR: &str = "\r\n";

impl fmt::Display for LineSeparator {
    fn fmt(&self, f: &mut fmt::Formatter) -> Result<(), fmt::Error> {
        f.write_str(self.as_str())
    }
}

impl LineSeparator {
    /// String representation
    pub fn as_str(&self) -> &'static str {
        match *self {
            LineSeparator::SystemDefault => DEFAULT_LINE_SEPARATOR,
            LineSeparator::CR => "\n",
            LineSeparator::CRLF => "\r\n",
        }
    }
}

/// Writing configuration
pub struct WriteOption {
    /// Policies about how to escape characters
    pub escape_policy: EscapePolicy,

    /// Newline style
    pub line_separator: LineSeparator,
}

impl Default for WriteOption {
    fn default() -> WriteOption {
        WriteOption { escape_policy: EscapePolicy::Basics,
                      line_separator: LineSeparator::SystemDefault }
    }
}

/// A setter which could be used to set key-value pair in a specified section
pub struct SectionSetter<'a> {
    ini: &'a mut Ini,
    section_name: Option<String>,
}

impl<'a> SectionSetter<'a> {
    fn new(ini: &'a mut Ini, section_name: Option<String>) -> SectionSetter<'a> {
        SectionSetter { ini, section_name }
    }

    /// Set key-value pair in this section
    pub fn set<K, V>(&'a mut self, key: K, value: V) -> &'a mut SectionSetter<'a>
        where K: Into<String>,
              V: Into<String>
    {
        {
            let prop = self.ini
                           .sections
                           .entry(self.section_name.clone())
                           .or_insert_with(Default::default);
            prop.insert(key.into(), value.into());
        }
        self
    }

    /// Delete the entry in this section with `key`
    pub fn delete<K: AsRef<str>>(&'a mut self, key: &K) -> &'a mut SectionSetter<'a> {
        if let Some(prop) = self.ini.sections.get_mut(&self.section_name) {
            prop.remove(key);
        }
        self
    }

    /// Get the entry in this section with `key`
    pub fn get<K: AsRef<str>>(&'a mut self, key: K) -> Option<&'a str> {
        self.ini
            .sections
            .get(&self.section_name)
            .and_then(|prop| prop.get(key))
            .map(|s| s.as_ref())
    }
}

#[cfg(not(feature = "preserve_order"))]
type PropertiesImpl<K, V> = MultiMap<K, V>;
#[cfg(feature = "preserve_order")]
type PropertiesImpl<K, V> = Vec<(K, V)>;

/// Properties type (key-value pairs)
#[derive(Clone, Default, Debug, PartialEq)]
pub struct Properties {
    data: PropertiesImpl<String, String>,
}

impl Properties {
    /// Create an instance
    pub fn new() -> Properties {
        Default::default()
    }

    /// Get the number of the properties
    pub fn len(&self) -> usize {
        self.data.len()
    }

    /// Get an iterator of the properties
    pub fn iter(&self) -> impl Iterator<Item = (&String, &String)> {
        self.data.iter().map(|(k, v)| (k, v))
    }

    /// Return true if property exist
    pub fn contains_key<S: AsRef<str>>(&self, s: S) -> bool {
        self.iter().find(|(k, _)| *k == s.as_ref()).is_some()
    }
}

#[cfg(not(feature = "preserve_order"))]
impl Properties {
    /// Insert (key, value) pair
    pub fn insert<K, V>(&mut self, k: K, v: V)
        where K: Into<String>,
              V: Into<String>
    {
        self.data.insert(k.into(), v.into());
    }

    /// Get the first value associate with the key
    pub fn get<S: AsRef<str>>(&self, s: S) -> Option<&String> {
        self.data.get(s.as_ref())
    }

    /// Get all values associate with the key
    pub fn get_vec<S: AsRef<str>>(&self, s: S) -> Option<Vec<&String>> {
        let ret = self.data.get_vec(s.as_ref())?.iter().collect();
        Some(ret)
    }

    /// Remove the property
    pub fn remove<S: AsRef<str>>(&mut self, s: S) -> Option<Vec<String>> {
        self.data.remove(s.as_ref())
    }

    fn get_mut<S: AsRef<str>>(&mut self, s: S) -> Option<&mut String> {
        self.data.get_mut(s.as_ref())
    }
}

#[cfg(feature = "preserve_order")]
impl Properties {
    /// Insert (key, value) pair
    pub fn insert<K, V>(&mut self, k: K, v: V)
        where K: Into<String>,
              V: Into<String>
    {
        self.data.push((k.into(), v.into()));
    }

    /// Get the first value associate with the key
    pub fn get<S: AsRef<str>>(&self, s: S) -> Option<&String> {
        for (k, v) in &self.data {
            if k == s.as_ref() {
                return Some(v);
            }
        }
        None
    }

    /// Get all values associate with the key
    pub fn get_vec<S: AsRef<str>>(&self, s: S) -> Option<Vec<&String>> {
        let ret: Vec<_> = self.data
                              .iter()
                              .filter(|(k, _)| k == s.as_ref())
                              .map(|(_, v)| v)
                              .collect();

        if ret.is_empty() {
            None
        } else {
            Some(ret)
        }
    }

    /// Remove the property
    pub fn remove<S: AsRef<str>>(&mut self, s: S) -> Option<Vec<String>> {
        let len = self.data.len();
        let mut data = Vec::with_capacity(len);
        let mut ret = Vec::with_capacity(len);

        std::mem::swap(&mut self.data, &mut data);

        for (k, v) in data {
            if k == s.as_ref() {
                ret.push(v);
            } else {
                self.data.push((k, v))
            }
        }

        if ret.is_empty() {
            None
        } else {
            Some(ret)
        }
    }

    fn get_mut<S: AsRef<str>>(&mut self, s: S) -> Option<&mut String> {
        for (k, v) in &mut self.data {
            if k == s.as_ref() {
                return Some(v);
            }
        }
        None
    }
}

impl<S: AsRef<str>> Index<S> for Properties {
    type Output = String;

    fn index(&self, index: S) -> &String {
        let s = index.as_ref();
        match self.get(s) {
            Some(p) => p,
            None => panic!("Key `{}` does not exist", s),
        }
    }
}

/// Ini struct
#[derive(Clone, Default)]
pub struct Ini {
    sections: Map<Option<String>, Properties>,
}

impl Ini {
    /// Create an instance
    pub fn new() -> Ini {
        Default::default()
    }

    /// Set with a specified section, `None` is for the general section
    pub fn with_section<S>(&mut self, section: Option<S>) -> SectionSetter
        where S: Into<String>
    {
        SectionSetter::new(self, section.map(Into::into))
    }

    /// Get the immmutable general section
    pub fn general_section(&self) -> &Properties {
        self.section(None::<String>)
            .expect("There is no general section in this Ini")
    }

    /// Get the mutable general section
    pub fn general_section_mut(&mut self) -> &mut Properties {
        self.section_mut(None::<String>)
            .expect("There is no general section in this Ini")
    }

    /// Get a immutable section
    pub fn section<S>(&self, name: Option<S>) -> Option<&Properties>
        where S: Into<String>
    {
        self.sections.get(&name.map(Into::into))
    }

    /// Get a mutable section
    pub fn section_mut<S>(&mut self, name: Option<S>) -> Option<&mut Properties>
        where S: Into<String>
    {
        self.sections.get_mut(&name.map(Into::into))
    }

    /// Get the entry
    pub fn entry(&mut self, name: Option<String>) -> Entry<Option<String>, Properties> {
        self.sections.entry(name.map(|s| s))
    }

    /// Clear all entries
    pub fn clear(&mut self) {
        self.sections.clear()
    }

    /// Iterate with sections
    pub fn sections(&self) -> Keys<Option<String>, Properties> {
        self.sections.keys()
    }

    /// Set key-value to a section
    pub fn set_to<S>(&mut self, section: Option<S>, key: String, value: String)
        where S: Into<String>
    {
        self.with_section(section).set(key, value);
    }

    /// Get the value from a section with key
    ///
    /// Example:
    ///
    /// ```
    /// use ini::Ini;
    /// let input = "[sec]\nabc = def\n";
    /// let ini = Ini::load_from_str(input).unwrap();
    /// assert_eq!(ini.get_from(Some("sec"), "abc"), Some("def"));
    /// ```
    pub fn get_from<'a, S>(&'a self, section: Option<S>, key: &str) -> Option<&'a str>
        where S: Into<String>
    {
        match self.sections.get(&section.map(Into::into)) {
            None => None,
            Some(ref prop) => match prop.get(key) {
                Some(p) => Some(&p[..]),
                None => None,
            },
        }
    }

    /// Get the value from a section with key, return the default value if it does not exist
    ///
    /// Example:
    ///
    /// ```
    /// use ini::Ini;
    /// let input = "[sec]\n";
    /// let ini = Ini::load_from_str(input).unwrap();
    /// assert_eq!(ini.get_from_or(Some("sec"), "key", "default"), "default");
    /// ```
    pub fn get_from_or<'a, S>(&'a self, section: Option<S>, key: &str, default: &'a str) -> &'a str
        where S: Into<String>
    {
        match self.sections.get(&section.map(Into::into)) {
            None => default,
            Some(ref prop) => match prop.get(key) {
                Some(p) => &p[..],
                None => default,
            },
        }
    }

    /// Get the mutable from a section with key
    pub fn get_from_mut<'a, S>(&'a mut self, section: Option<S>, key: &str) -> Option<&'a str>
        where S: Into<String>
    {
        match self.sections.get_mut(&section.map(Into::into)) {
            None => None,
            Some(prop) => prop.get_mut(key).map(|s| &s[..]),
        }
    }

    /// Delete a section, return the properties if it exists
    pub fn delete<S>(&mut self, section: Option<S>) -> Option<Properties>
        where S: Into<String>
    {
        self.sections.remove(&section.map(Into::into))
    }

    pub fn delete_from<S>(&mut self, section: Option<S>, key: &str) -> Option<Vec<String>>
        where S: Into<String>
    {
        self.section_mut(section).and_then(|prop| prop.remove(key))
    }
}

impl<'q> Index<&'q Option<String>> for Ini {
    type Output = Properties;

    fn index<'a>(&'a self, index: &'q Option<String>) -> &'a Properties {
        match self.sections.get(index) {
            Some(p) => p,
            None => panic!("Section `{:?}` does not exist", index),
        }
    }
}

impl<'i> IndexMut<&'i Option<String>> for Ini {
    fn index_mut<'a>(&'a mut self, index: &Option<String>) -> &'a mut Properties {
        match self.sections.get_mut(index) {
            Some(p) => p,
            None => panic!("Section `{:?}` does not exist", index),
        }
    }
}

impl<'q> Index<&'q str> for Ini {
    type Output = Properties;

    fn index<'a>(&'a self, index: &'q str) -> &'a Properties {
        match self.sections.get(&Some(index.into())) {
            Some(p) => p,
            None => panic!("Section `{}` does not exist", index),
        }
    }
}

impl<'q> IndexMut<&'q str> for Ini {
    fn index_mut<'a>(&'a mut self, index: &'q str) -> &'a mut Properties {
        match self.sections.get_mut(&Some(index.into())) {
            Some(p) => p,
            None => panic!("Section `{}` does not exist", index),
        }
    }
}

impl Ini {
    /// Write to a file
    pub fn write_to_file<P: AsRef<Path>>(&self, filename: P) -> io::Result<()> {
        self.write_to_file_policy(filename, EscapePolicy::Basics)
    }

    /// Write to a file
    pub fn write_to_file_policy<P: AsRef<Path>>(&self, filename: P, policy: EscapePolicy) -> io::Result<()> {
        let mut file = OpenOptions::new().write(true)
                                         .truncate(true)
                                         .create(true)
                                         .open(filename.as_ref())?;
        self.write_to_policy(&mut file, policy)
    }

    /// Write to a file with options
    pub fn write_to_file_opt<P: AsRef<Path>>(&self, filename: P, opt: WriteOption) -> io::Result<()> {
        let mut file = OpenOptions::new().write(true)
                                         .truncate(true)
                                         .create(true)
                                         .open(filename.as_ref())?;
        self.write_to_opt(&mut file, opt)
    }

    /// Write to a writer
    pub fn write_to<W: Write>(&self, writer: &mut W) -> io::Result<()> {
        self.write_to_opt(writer, Default::default())
    }

    /// Write to a writer
    pub fn write_to_policy<W: Write>(&self, writer: &mut W, policy: EscapePolicy) -> io::Result<()> {
        self.write_to_opt(writer,
                          WriteOption { escape_policy: policy,
                                        ..Default::default() })
    }

    /// Write to a writer with options
    pub fn write_to_opt<W: Write>(&self, writer: &mut W, opt: WriteOption) -> io::Result<()> {
        let mut firstline = true;

        if let Some(props) = self.sections.get(&None) {
            for (k, v) in props.iter() {
                let k_str = escape_str(&k[..], opt.escape_policy);
                let v_str = escape_str(&v[..], opt.escape_policy);
                write!(writer, "{}={}{}", k_str, v_str, opt.line_separator)?;
            }
            firstline = false;
        }

        for (section, props) in self.sections.iter().filter(|&(ref s, _)| s.is_some()) {
            if firstline {
                firstline = false;
            } else {
                writer.write_all(opt.line_separator.as_str().as_bytes())?;
            }

            if let Some(ref section) = *section {
                write!(writer,
                       "[{}]{}",
                       escape_str(&section[..], opt.escape_policy),
                       opt.line_separator)?;

                for (k, v) in props.iter() {
                    let k_str = escape_str(&k[..], opt.escape_policy);
                    let v_str = escape_str(&v[..], opt.escape_policy);
                    write!(writer, "{}={}{}", k_str, v_str, opt.line_separator)?;
                }
            }
        }
        Ok(())
    }
}

impl Ini {
    /// Load from a string
    pub fn load_from_str(buf: &str) -> Result<Ini, ParseError> {
        Ini::load_from_str_opt(buf, ParseOption::default())
    }

    /// Load from a string, but do not interpret '\' as an escape character
    pub fn load_from_str_noescape(buf: &str) -> Result<Ini, ParseError> {
        Ini::load_from_str_opt(buf,
                               ParseOption { enabled_escape: false,
                                             ..ParseOption::default() })
    }

    /// Load from a string with options
    pub fn load_from_str_opt(buf: &str, opt: ParseOption) -> Result<Ini, ParseError> {
        let mut parser = Parser::new(buf.chars(), opt);
        parser.parse()
    }

    /// Load from a reader
    pub fn read_from<R: Read>(reader: &mut R) -> Result<Ini, Error> {
        Ini::read_from_opt(reader, ParseOption::default())
    }

    /// Load from a reader, but do not interpret '\' as an escape character
    pub fn read_from_noescape<R: Read>(reader: &mut R) -> Result<Ini, Error> {
        Ini::read_from_opt(reader,
                           ParseOption { enabled_escape: false,
                                         ..ParseOption::default() })
    }

    /// Load from a reader with options
    pub fn read_from_opt<R: Read>(reader: &mut R, opt: ParseOption) -> Result<Ini, Error> {
        let mut s = String::new();
        reader.read_to_string(&mut s).map_err(Error::Io)?;
        let mut parser = Parser::new(s.chars(), opt);
        match parser.parse() {
            Err(e) => Err(Error::Parse(e)),
            Ok(success) => Ok(success),
        }
    }

    /// Load from a file
    pub fn load_from_file<P: AsRef<Path>>(filename: P) -> Result<Ini, Error> {
        Ini::load_from_file_opt(filename, ParseOption::default())
    }

    /// Load from a file, but do not interpret '\' as an escape character
    pub fn load_from_file_noescape<P: AsRef<Path>>(filename: P) -> Result<Ini, Error> {
        Ini::load_from_file_opt(filename,
                                ParseOption { enabled_escape: false,
                                              ..ParseOption::default() })
    }

    /// Load from a file with options
    pub fn load_from_file_opt<P: AsRef<Path>>(filename: P, opt: ParseOption) -> Result<Ini, Error> {
        let mut reader = match File::open(filename.as_ref()) {
            Err(e) => {
                return Err(Error::Io(e));
            }
            Ok(r) => r,
        };
        Ini::read_from_opt(&mut reader, opt)
    }
}

/// Iterator for sections
pub struct SectionIterator<'a> {
    mapiter: Iter<'a, Option<String>, Properties>,
}

/// Iterator for mutable sections
pub struct SectionMutIterator<'a> {
    mapiter: IterMut<'a, Option<String>, Properties>,
}

impl<'a> Ini {
    /// Immutable iterate though sections
    pub fn iter(&'a self) -> SectionIterator<'a> {
        SectionIterator { mapiter: self.sections.iter() }
    }

    /// Mutable iterate though sections
    /// *Deprecated! Use `iter_mut` instead!*
    pub fn mut_iter(&'a mut self) -> SectionMutIterator<'a> {
        SectionMutIterator { mapiter: self.sections.iter_mut() }
    }

    /// Mutable iterate though sections
    pub fn iter_mut(&'a mut self) -> SectionMutIterator<'a> {
        SectionMutIterator { mapiter: self.sections.iter_mut() }
    }
}

impl<'a> Iterator for SectionIterator<'a> {
    type Item = (&'a Option<String>, &'a Properties);

    #[inline]
    fn next(&mut self) -> Option<(&'a Option<String>, &'a Properties)> {
        self.mapiter.next()
    }
}

impl<'a> Iterator for SectionMutIterator<'a> {
    type Item = (&'a Option<String>, &'a mut Properties);

    #[inline]
    fn next(&mut self) -> Option<(&'a Option<String>, &'a mut Properties)> {
        self.mapiter.next()
    }
}

impl<'a> IntoIterator for &'a Ini {
    type Item = (&'a Option<String>, &'a Properties);
    type IntoIter = SectionIterator<'a>;

    fn into_iter(self) -> SectionIterator<'a> {
        self.iter()
    }
}

impl<'a> IntoIterator for &'a mut Ini {
    type Item = (&'a Option<String>, &'a mut Properties);
    type IntoIter = SectionMutIterator<'a>;

    fn into_iter(self) -> SectionMutIterator<'a> {
        self.iter_mut()
    }
}

pub struct SectionIntoIter {
    iter: IntoIter<Option<String>, Properties>,
}

impl Iterator for SectionIntoIter {
    type Item = (Option<String>, Properties);
    fn next(&mut self) -> Option<Self::Item> {
        self.iter.next()
    }
}

impl IntoIterator for Ini {
    type Item = (Option<String>, Properties);
    type IntoIter = SectionIntoIter;

    fn into_iter(self) -> SectionIntoIter {
        SectionIntoIter { iter: self.sections.into_iter() }
    }
}

// Ini parser
struct Parser<'a> {
    ch: Option<char>,
    rdr: Chars<'a>,
    line: usize,
    col: usize,
    opt: ParseOption,
}

#[derive(Debug)]
/// Parse error
pub struct ParseError {
    pub line: usize,
    pub col: usize,
    pub msg: String,
}

impl Display for ParseError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "{}:{} {}", self.line, self.col, self.msg)
    }
}

impl error::Error for ParseError {}

#[derive(Debug)]
pub enum Error {
    Io(io::Error),
    Parse(ParseError),
}

impl Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match *self {
            Error::Io(ref err) => err.fmt(f),
            Error::Parse(ref err) => err.fmt(f),
        }
    }
}

impl error::Error for Error {
    fn source(&self) -> Option<&(dyn error::Error + 'static)> {
        match *self {
            Error::Io(ref err) => err.source(),
            Error::Parse(ref err) => err.source(),
        }
    }
}

impl<'a> Parser<'a> {
    // Create a parser
    pub fn new(rdr: Chars<'a>, opt: ParseOption) -> Parser<'a> {
        let mut p = Parser { ch: None,
                             line: 0,
                             col: 0,
                             rdr,
                             opt };
        p.bump();
        p
    }

    fn eof(&self) -> bool {
        self.ch.is_none()
    }

    fn bump(&mut self) {
        self.ch = self.rdr.next();
        match self.ch {
            Some('\n') => {
                self.line += 1;
                self.col = 0;
            }
            Some(..) => {
                self.col += 1;
            }
            None => {}
        }
    }

    fn error<U, M: Into<String>>(&self, msg: M) -> Result<U, ParseError> {
        Err(ParseError { line: self.line,
                         col: self.col,
                         msg: msg.into() })
    }

    /// Consume all the white space until the end of the line or a tab
    fn parse_whitespace(&mut self) {
        while let Some(c) = self.ch {
            if !c.is_whitespace() && c != '\n' && c != '\t' && c != '\r' {
                break;
            }
            self.bump();
        }
    }

    /// Consume all the white space except line break
    fn parse_whitespace_except_line_break(&mut self) {
        while let Some(c) = self.ch {
            if (c == '\n' || c == '\r' || !c.is_whitespace()) && c != '\t' {
                break;
            }
            self.bump();
        }
    }

    /// Parse the whole INI input
    pub fn parse(&mut self) -> Result<Ini, ParseError> {
        let mut result = Ini::new();
        let mut curkey: String = "".into();
        let mut cursec: Option<String> = None;

        self.parse_whitespace();
        while let Some(cur_ch) = self.ch {
            match cur_ch {
                ';' | '#' => {
                    if cfg!(not(feature = "inline_comment")) {
                        // Inline comments is not supported, so comments must starts from a new line
                        //
                        // https://en.wikipedia.org/wiki/INI_file#Comments
                        if self.col > 1 {
                            return self.error("doesn't support inline comment");
                        }
                    }

                    self.parse_comment();
                }
                '[' => match self.parse_section() {
                    Ok(sec) => {
                        let msec = &sec[..].trim();
                        cursec = Some(msec.to_string());
                        result.sections.entry(cursec.clone()).or_insert_with(Default::default);
                        self.bump();
                    }
                    Err(e) => return Err(e),
                },
                '=' | ':' => {
                    if (&curkey[..]).is_empty() {
                        return self.error("missing key");
                    }
                    match self.parse_val() {
                        Ok(val) => {
                            let mval = val[..].trim().to_owned();
                            let sec = result.sections.entry(cursec.clone()).or_insert_with(Default::default);
                            sec.insert(curkey, mval);
                            curkey = "".into();
                        }
                        Err(e) => return Err(e),
                    }
                }
                _ => match self.parse_key() {
                    Ok(key) => {
                        let mkey: String = key[..].trim().to_owned();
                        curkey = mkey;
                    }
                    Err(e) => return Err(e),
                },
            }

            self.parse_whitespace();
        }

        Ok(result)
    }

    fn parse_comment(&mut self) {
        while let Some(c) = self.ch {
            self.bump();
            if c == '\n' {
                break;
            }
        }
    }

    fn parse_str_until(&mut self, endpoint: &[Option<char>]) -> Result<String, ParseError> {
        let mut result: String = String::new();

        while !endpoint.contains(&self.ch) {
            match self.ch {
                None => {
                    return self.error(format!("expecting \"{:?}\" but found EOF.", endpoint));
                }
                Some('\\') if self.opt.enabled_escape => {
                    self.bump();
                    if self.eof() {
                        return self.error(format!("expecting \"{:?}\" but found EOF.", endpoint));
                    }
                    match self.ch.unwrap() {
                        '0' => result.push('\0'),
                        'a' => result.push('\x07'),
                        'b' => result.push('\x08'),
                        't' => result.push('\t'),
                        'r' => result.push('\r'),
                        'n' => result.push('\n'),
                        '\n' => (),
                        'x' => {
                            // Unicode 4 character
                            let mut code: String = String::with_capacity(4);
                            for _ in 0..4 {
                                self.bump();
                                if self.eof() {
                                    return self.error(format!("expecting \"{:?}\" but found EOF.", endpoint));
                                } else if let Some('\\') = self.ch {
                                    self.bump();
                                    if self.ch != Some('\n') {
                                        return self.error(format!("expecting \"\\\\n\" but \
                                                                   found \"{:?}\".",
                                                                  self.ch));
                                    }
                                }
                                code.push(self.ch.unwrap());
                            }
                            let r = u32::from_str_radix(&code[..], 16);
                            match r {
                                Ok(c) => result.push(char::from_u32(c).unwrap()),
                                Err(_) => return self.error("unknown character in \\xHH form"),
                            }
                        }
                        c => result.push(c),
                    }
                }
                Some(c) => {
                    result.push(c);
                }
            }
            self.bump();
        }
        Ok(result)
    }

    fn parse_section(&mut self) -> Result<String, ParseError> {
        // Skip [
        self.bump();
        self.parse_str_until(&[Some(']')])
    }

    fn parse_key(&mut self) -> Result<String, ParseError> {
        self.parse_str_until(&[Some('='), Some(':')])
    }

    fn parse_val(&mut self) -> Result<String, ParseError> {
        self.bump();
        // Issue #35: Allow empty value
        self.parse_whitespace_except_line_break();

        match self.ch {
            None => Ok(String::new()),
            Some('"') if self.opt.enabled_quote => {
                self.bump();
                self.parse_str_until(&[Some('"')]).and_then(|s| {
                                                      self.bump(); // Eats the last "
                                                                   // Parse until EOL
                                                      self.parse_str_until_eol().map(|x| s + &x)
                                                  })
            }
            Some('\'') if self.opt.enabled_quote => {
                self.bump();
                self.parse_str_until(&[Some('\'')]).and_then(|s| {
                                                       self.bump(); // Eats the last '
                                                                    // Parse until EOL
                                                       self.parse_str_until_eol().map(|x| s + &x)
                                                   })
            }
            _ => self.parse_str_until_eol(),
        }
    }

    #[cfg(not(feature = "inline_comment"))]
    fn parse_str_until_eol(&mut self) -> Result<String, ParseError> {
        self.parse_str_until(&[Some('\n'), Some('\r'), None])
    }

    #[cfg(feature = "inline_comment")]
    fn parse_str_until_eol(&mut self) -> Result<String, ParseError> {
        self.parse_str_until(&[Some('\n'), Some('\r'), Some(';'), Some('#'), None])
    }
}

// ------------------------------------------------------------------------------

#[cfg(test)]
mod test {
    use super::*;

    #[test]
    fn test_property_get_vec() {
        let mut props = Properties::new();
        props.insert("k1", "v1");
        props.insert("k1", "v2");

        let mut res = props.get_vec("k1").unwrap();
        res.sort();
        assert_eq!(res, vec!["v1", "v2"]);

        let res = props.get_vec("k2");
        assert_eq!(res, None);
    }

    #[test]
    fn test_property_remove() {
        let mut props = Properties::new();
        props.insert("k1", "v1");
        props.insert("k1", "v2");

        let mut res = props.remove("k1").unwrap();
        res.sort();
        assert_eq!(res, vec!["v1", "v2"]);
        assert!(!props.contains_key("k1"));
    }

    #[test]
    fn load_from_str_with_valid_input() {
        let input = "[sec1]\nkey1=val1\nkey2=377\n[sec2]foo=bar\n";
        let opt = Ini::load_from_str(input);
        assert!(opt.is_ok());

        let output = opt.unwrap();
        assert_eq!(output.sections.len(), 2);
        assert!(output.sections.contains_key(&Some("sec1".into())));

        let sec1 = &output.sections[&Some("sec1".into())];
        assert_eq!(sec1.len(), 2);
        let key1: String = "key1".into();
        assert!(sec1.contains_key(&key1));
        let key2: String = "key2".into();
        assert!(sec1.contains_key(&key2));
        let val1: String = "val1".into();
        assert_eq!(sec1[&key1], val1);
        let val2: String = "377".into();
        assert_eq!(sec1[&key2], val2);
    }

    #[test]
    fn load_from_str_without_ending_newline() {
        let input = "[sec1]\nkey1=val1\nkey2=377\n[sec2]foo=bar";
        let opt = Ini::load_from_str(input);
        assert!(opt.is_ok());
    }

    #[test]
    fn test_parse_comment() {
        let input = "; abcdefghijklmn\n";
        let opt = Ini::load_from_str(input);
        assert!(opt.is_ok());
    }

    #[cfg(not(feature = "inline_comment"))]
    #[test]
    fn test_inline_comment_not_supported() {
        let input = "
[section name]
name = hello # abcdefg
gender = mail ; abdddd
";
        let ini = Ini::load_from_str(input).unwrap();
        assert_eq!(ini.get_from(Some("section name"), "name").unwrap(), "hello # abcdefg");
        assert_eq!(ini.get_from(Some("section name"), "gender").unwrap(), "mail ; abdddd");
    }

    #[test]
    #[cfg_attr(not(feature = "inline_comment"), should_panic)]
    fn test_inline_comment() {
        let input = "
[section name] # comment in section line
name = hello # abcdefg
gender = mail ; abdddd
";
        let ini = Ini::load_from_str(input).unwrap();
        assert_eq!(ini.get_from(Some("section name"), "name").unwrap(), "hello");
        assert_eq!(ini.get_from(Some("section name"), "gender").unwrap(), "mail");
    }

    #[test]
    fn test_sharp_comment() {
        let input = "
[section name]
name = hello
# abcdefg
";
        let ini = Ini::load_from_str(input).unwrap();
        assert_eq!(ini.get_from(Some("section name"), "name").unwrap(), "hello");
    }

    #[test]
    fn test_iter() {
        let input = "
[section name]
name = hello # abcdefg
gender = mail ; abdddd
";
        let mut ini = Ini::load_from_str(input).unwrap();

        for _ in &mut ini {}
        for _ in &ini {}
        for _ in ini {}
    }

    #[test]
    fn test_colon() {
        let input = "
[section name]
name: hello
gender : mail
";
        let ini = Ini::load_from_str(input).unwrap();
        assert_eq!(ini.get_from(Some("section name"), "name").unwrap(), "hello");
        assert_eq!(ini.get_from(Some("section name"), "gender").unwrap(), "mail");
    }

    #[test]
    fn test_string() {
        let input = "
[section name]
# This is a comment
Key = \"Value\"
";
        let ini = Ini::load_from_str(input).unwrap();
        assert_eq!(ini.get_from(Some("section name"), "Key").unwrap(), "Value");
    }

    #[test]
    fn test_string_multiline() {
        let input = "
[section name]
# This is a comment
Key = \"Value
Otherline\"
";
        let ini = Ini::load_from_str(input).unwrap();
        assert_eq!(ini.get_from(Some("section name"), "Key").unwrap(), "Value\nOtherline");
    }

    #[test]
    fn test_string_comment() {
        let input = "
[section name]
# This is a comment
Key = \"Value   # This is not a comment ; at all\"
Stuff = Other
";
        let ini = Ini::load_from_str(input).unwrap();
        assert_eq!(ini.get_from(Some("section name"), "Key").unwrap(),
                   "Value   # This is not a comment ; at all");
    }

    #[test]
    fn test_string_single() {
        let input = "
[section name]
# This is a comment
Key = 'Value'
Stuff = Other
";
        let ini = Ini::load_from_str(input).unwrap();
        assert_eq!(ini.get_from(Some("section name"), "Key").unwrap(), "Value");
    }

    #[test]
    fn test_string_includes_quote() {
        let input = "
[Test]
Comment[tr]=İnternet'e erişin
Comment[uk]=Доступ до Інтернету
";
        let ini = Ini::load_from_str(input).unwrap();
        assert_eq!(ini.get_from(Some("Test"), "Comment[tr]").unwrap(), "İnternet'e erişin");
    }

    #[test]
    fn test_string_single_multiline() {
        let input = "
[section name]
# This is a comment
Key = 'Value
Otherline'
Stuff = Other
";
        let ini = Ini::load_from_str(input).unwrap();
        assert_eq!(ini.get_from(Some("section name"), "Key").unwrap(), "Value\nOtherline");
    }

    #[test]
    fn test_string_single_comment() {
        let input = "
[section name]
# This is a comment
Key = 'Value   # This is not a comment ; at all'
";
        let ini = Ini::load_from_str(input).unwrap();
        assert_eq!(ini.get_from(Some("section name"), "Key").unwrap(),
                   "Value   # This is not a comment ; at all");
    }

    #[test]
    fn load_from_str_with_valid_empty_input() {
        let input = "key1=\nkey2=val2\n";
        let opt = Ini::load_from_str(input);
        assert!(opt.is_ok());

        let output = opt.unwrap();
        assert_eq!(output.sections.len(), 1);
        assert!(output.sections.contains_key(&None::<String>));

        let sec1 = &output.sections[&None::<String>];
        assert_eq!(sec1.len(), 2);
        let key1: String = "key1".into();
        assert!(sec1.contains_key(&key1));
        let key2: String = "key2".into();
        assert!(sec1.contains_key(&key2));
        let val1: String = "".into();
        assert_eq!(sec1[&key1], val1);
        let val2: String = "val2".into();
        assert_eq!(sec1[&key2], val2);
    }

    #[test]
    fn load_from_str_with_crlf() {
        let input = "key1=val1\r\nkey2=val2\r\n";
        let opt = Ini::load_from_str(input);
        assert!(opt.is_ok());

        let output = opt.unwrap();
        assert_eq!(output.sections.len(), 1);
        assert!(output.sections.contains_key(&None::<String>));
        let sec1 = &output.sections[&None::<String>];
        assert_eq!(sec1.len(), 2);
        let key1: String = "key1".into();
        assert!(sec1.contains_key(&key1));
        let key2: String = "key2".into();
        assert!(sec1.contains_key(&key2));
        let val1: String = "val1".into();
        assert_eq!(sec1[&key1], val1);
        let val2: String = "val2".into();
        assert_eq!(sec1[&key2], val2);
    }

    #[test]
    fn load_from_str_with_cr() {
        let input = "key1=val1\rkey2=val2\r";
        let opt = Ini::load_from_str(input);
        assert!(opt.is_ok());

        let output = opt.unwrap();
        assert_eq!(output.sections.len(), 1);
        assert!(output.sections.contains_key(&None::<String>));
        let sec1 = &output.sections[&None::<String>];
        assert_eq!(sec1.len(), 2);
        let key1: String = "key1".into();
        assert!(sec1.contains_key(&key1));
        let key2: String = "key2".into();
        assert!(sec1.contains_key(&key2));
        let val1: String = "val1".into();
        assert_eq!(sec1[&key1], val1);
        let val2: String = "val2".into();
        assert_eq!(sec1[&key2], val2);
    }

    #[test]
    fn get_with_non_static_key() {
        let input = "key1=val1\nkey2=val2\n";
        let opt = Ini::load_from_str(input).unwrap();

        let sec1 = &opt.sections[&None::<String>];

        let key = "key1".to_owned();
        sec1.get(&key).unwrap();
    }

    #[test]
    fn load_from_str_noescape() {
        let input = "path=C:\\Windows\\Some\\Folder\\";
        let opt = Ini::load_from_str_noescape(input);
        assert!(opt.is_ok());

        let output = opt.unwrap();
        assert_eq!(output.sections.len(), 1);
        let sec = &output.sections[&None::<String>];
        assert_eq!(sec.len(), 1);
        assert!(sec.contains_key("path"));
        assert_eq!(sec["path"], "C:\\Windows\\Some\\Folder\\");
    }

    #[test]
    fn partial_quoting_double() {
        let input = "
[Section]
A=\"quote\" arg0
B=b";

        let opt = Ini::load_from_str(input).unwrap();
        let sec = opt.section(Some("Section")).unwrap();
        assert_eq!(sec["A"], "quote arg0");
        assert_eq!(sec["B"], "b");
    }

    #[test]
    fn partial_quoting_single() {
        let input = "
[Section]
A='quote' arg0
B=b";

        let opt = Ini::load_from_str(input).unwrap();
        let sec = opt.section(Some("Section")).unwrap();
        assert_eq!(sec["A"], "quote arg0");
        assert_eq!(sec["B"], "b");
    }

    #[test]
    fn parse_without_quote() {
        let input = "
[Desktop Entry]
Exec = \"/path/to/exe with space\" arg
";

        let opt = Ini::load_from_str_opt(input,
                                         ParseOption { enabled_quote: false,
                                                       ..ParseOption::default() }).unwrap();
        let sec = opt.section(Some("Desktop Entry")).unwrap();
        assert_eq!(sec["Exec"], "\"/path/to/exe with space\" arg");
    }
}

#[cfg(test)]
#[cfg(feature = "preserve_order")]
mod preserve_order {
    use super::*;

    #[test]
    fn test_preserve_order_section() {
        let input = r"
none2 = n2
[SB]
p2 = 2
[SA]
x2 = 2
[SC]
cd1 = x
[xC]
xd = x
        ";

        let data = Ini::load_from_str(input).unwrap();
        let keys: Vec<Option<&str>> = data.iter().map(|(k, _)| k.as_ref().map(|s| s.as_ref())).collect();

        assert_eq!(keys.len(), 5);
        assert_eq!(keys[0], None);
        assert_eq!(keys[1], Some("SB"));
        assert_eq!(keys[2], Some("SA"));
        assert_eq!(keys[3], Some("SC"));
        assert_eq!(keys[4], Some("xC"));
    }

    #[test]
    fn test_preserve_order_property() {
        let input = r"
x2 = n2
x1 = n2
x3 = n2
";
        let data = Ini::load_from_str(input).unwrap();
        let section = data.general_section();
        let keys: Vec<&str> = section.iter().map(|(k, _)| k.as_ref()).collect();
        assert_eq!(keys, vec!["x2", "x1", "x3"]);
    }

    #[test]
    fn test_preserve_order_property_in_section() {
        let input = r"
[s]
x2 = n2
xb = n2
a3 = n3
";
        let data = Ini::load_from_str(input).unwrap();
        let section = data.section(Some("s")).unwrap();
        let keys: Vec<&str> = section.iter().map(|(k, _)| k.as_ref()).collect();
        assert_eq!(keys, vec!["x2", "xb", "a3"])
    }

    #[test]
    fn test_preserve_order_write() {
        let input = r"
x2 = n2
x1 = n2
x3 = n2
[s]
x2 = n2
xb = n2
a3 = n3
";
        let data = Ini::load_from_str(input).unwrap();
        let mut buf = vec![];
        data.write_to(&mut buf).unwrap();
        let new_data = Ini::load_from_str(&String::from_utf8(buf).unwrap()).unwrap();

        let sec0 = new_data.general_section();
        let keys0: Vec<&str> = sec0.iter().map(|(k, _)| k.as_ref()).collect();
        assert_eq!(keys0, vec!["x2", "x1", "x3"]);

        let sec1 = new_data.section(Some("s")).unwrap();
        let keys1: Vec<&str> = sec1.iter().map(|(k, _)| k.as_ref()).collect();
        assert_eq!(keys1, vec!["x2", "xb", "a3"]);
    }

    #[test]
    fn write_line_separator() {
        use std::str;

        let mut ini = Ini::new();
        ini.with_section(Some("Section1"))
           .set("Key1", "Value")
           .set("Key2", "Value");
        ini.with_section(Some("Section2"))
           .set("Key1", "Value")
           .set("Key2", "Value");

        {
            let mut buf = Vec::new();
            ini.write_to_opt(&mut buf,
                             WriteOption { line_separator: LineSeparator::CR,
                                           ..Default::default() })
               .unwrap();

            assert_eq!("[Section1]\nKey1=Value\nKey2=Value\n\n[Section2]\nKey1=Value\nKey2=Value\n",
                       str::from_utf8(&buf).unwrap());
        }

        {
            let mut buf = Vec::new();
            ini.write_to_opt(&mut buf,
                             WriteOption { line_separator: LineSeparator::CRLF,
                                           ..Default::default() })
               .unwrap();

            assert_eq!("[Section1]\r\nKey1=Value\r\nKey2=Value\r\n\r\n[Section2]\r\nKey1=Value\r\nKey2=Value\r\n",
                       str::from_utf8(&buf).unwrap());
        }

        {
            let mut buf = Vec::new();
            ini.write_to_opt(&mut buf,
                             WriteOption { line_separator: LineSeparator::SystemDefault,
                                           ..Default::default() })
               .unwrap();

            if cfg!(windows) {
                assert_eq!("[Section1]\r\nKey1=Value\r\nKey2=Value\r\n\r\n[Section2]\r\nKey1=Value\r\nKey2=Value\r\n",
                           str::from_utf8(&buf).unwrap());
            } else {
                assert_eq!("[Section1]\nKey1=Value\nKey2=Value\n\n[Section2]\nKey1=Value\nKey2=Value\n",
                           str::from_utf8(&buf).unwrap());
            }
        }
    }
}
