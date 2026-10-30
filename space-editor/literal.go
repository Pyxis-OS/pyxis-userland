// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

package main

// Literal configuration data: the "return { ... }" subset that
// userspace/lib/literal.c accepts and boot reads. Nothing is evaluated.
// Entries keep their source order so carried-through values are written back
// in place; comments are dropped.

import (
	"fmt"
	"math"
	"strconv"
	"strings"
)

// The table depth is the only bound: it limits recursion. The source size and
// the number of values and entries are not limited, as in the C parser's sized
// entry point; memory is the bound.
const literalDepthLimit = 32

type kind int

const (
	tableKind kind = iota
	stringKind
	integerKind
	numberKind
	booleanKind
)

type value struct {
	kind    kind
	text    string
	integer int64
	number  float64
	boolean bool
	entries []entry
}

type entry struct {
	key      *value
	value    *value
	implicit bool
}

type parseError struct {
	line, column int
	reason       string
}

func (e *parseError) Error() string {
	return fmt.Sprintf("line %d column %d: %s", e.line, e.column, e.reason)
}

type parser struct {
	source          string
	position        int
	failure         string
	failurePosition int
}

func (p *parser) fail(reason string) bool {
	if p.failure == "" {
		p.failure = reason
		p.failurePosition = p.position
	}
	return false
}

func (p *parser) peek() byte {
	if p.position < len(p.source) {
		return p.source[p.position]
	}
	return 0
}

func isSpace(b byte) bool {
	return b == ' ' || b == '\t' || b == '\r' || b == '\n' || b == '\v' || b == '\f'
}

func isDigit(b byte) bool { return b >= '0' && b <= '9' }

func isIdentifierStart(b byte) bool {
	return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || b == '_'
}

func isIdentifierPart(b byte) bool { return isIdentifierStart(b) || isDigit(b) }

func hexDigit(b byte) int {
	switch {
	case isDigit(b):
		return int(b - '0')
	case b >= 'a' && b <= 'f':
		return int(b-'a') + 10
	case b >= 'A' && b <= 'F':
		return int(b-'A') + 10
	}
	return -1
}

// bracket returns the end of an opening or closing long bracket at start, or
// start itself when there is none.
func (p *parser) bracket(start int, delimiter byte) int {
	if start >= len(p.source) || p.source[start] != delimiter {
		return start
	}
	end := start + 1
	for end < len(p.source) && p.source[end] == '=' {
		end++
	}
	if end < len(p.source) && p.source[end] == delimiter {
		return end + 1
	}
	return start
}

func (p *parser) longBody() (string, bool) {
	start := p.position
	openEnd := p.bracket(start, '[')
	if openEnd == start {
		return "", p.fail("invalid long bracket")
	}
	p.position = openEnd
	for p.position < len(p.source) {
		closeEnd := p.bracket(p.position, ']')
		if closeEnd-p.position == openEnd-start {
			body := p.source[openEnd:p.position]
			p.position = closeEnd
			return body, true
		}
		p.position++
	}
	return "", p.fail("unterminated long string/comment")
}

func (p *parser) skip() bool {
	for p.position < len(p.source) {
		switch {
		case isSpace(p.peek()):
			p.position++
		case p.peek() == '-' && p.position+1 < len(p.source) && p.source[p.position+1] == '-':
			p.position += 2
			if p.bracket(p.position, '[') != p.position {
				if _, ok := p.longBody(); !ok {
					return false
				}
			} else {
				for p.position < len(p.source) && p.peek() != '\n' && p.peek() != '\r' {
					p.position++
				}
			}
		default:
			return true
		}
	}
	return true
}

func (p *parser) newValue(k kind) *value {
	return &value{kind: k}
}

func (p *parser) newline() {
	first := p.peek()
	p.position++
	if (first == '\r' && p.peek() == '\n') || (first == '\n' && p.peek() == '\r') {
		p.position++
	}
}

func (p *parser) quoted() (string, bool) {
	quote := p.peek()
	p.position++
	var text strings.Builder
	for p.position < len(p.source) {
		b := p.peek()
		p.position++
		if b == quote {
			return text.String(), true
		}
		if b == '\r' || b == '\n' {
			return "", p.fail("newline in quoted string")
		}
		if b != '\\' {
			text.WriteByte(b)
			continue
		}
		if p.position == len(p.source) {
			break
		}
		b = p.peek()
		p.position++
		switch b {
		case 'a':
			text.WriteByte('\a')
		case 'b':
			text.WriteByte('\b')
		case 'f':
			text.WriteByte('\f')
		case 'n':
			text.WriteByte('\n')
		case 'r':
			text.WriteByte('\r')
		case 't':
			text.WriteByte('\t')
		case 'v':
			text.WriteByte('\v')
		case '\\', '\'', '"':
			text.WriteByte(b)
		case '\r', '\n':
			p.position--
			p.newline()
			text.WriteByte('\n')
		case 'z':
			for isSpace(p.peek()) {
				p.position++
			}
		case 'x':
			high := hexDigit(p.peek())
			if high < 0 {
				return "", p.fail("hex escape needs two digits")
			}
			p.position++
			low := hexDigit(p.peek())
			if low < 0 {
				return "", p.fail("hex escape needs two digits")
			}
			p.position++
			text.WriteByte(byte(high*16 + low))
		case 'u':
			if p.peek() != '{' {
				return "", p.fail("Unicode escape needs braces")
			}
			p.position++
			var codepoint uint32
			digits := 0
			for hexDigit(p.peek()) >= 0 {
				if codepoint > (0x7fffffff-uint32(hexDigit(p.peek())))/16 {
					return "", p.fail("Unicode escape is out of range")
				}
				codepoint = codepoint*16 + uint32(hexDigit(p.peek()))
				p.position++
				digits++
			}
			if digits == 0 || p.peek() != '}' {
				return "", p.fail("invalid Unicode escape")
			}
			p.position++
			text.WriteString(luaUTF8(codepoint))
		default:
			if !isDigit(b) {
				return "", p.fail("unsupported string escape")
			}
			number := int(b - '0')
			for i := 1; i < 3 && isDigit(p.peek()); i++ {
				number = number*10 + int(p.peek()-'0')
				p.position++
			}
			if number > 255 {
				return "", p.fail("decimal escape is out of range")
			}
			text.WriteByte(byte(number))
		}
	}
	return "", p.fail("unterminated quoted string")
}

// luaUTF8 encodes like Lua's \u{...}, which accepts values up to 2^31 - 1.
func luaUTF8(codepoint uint32) string {
	if codepoint < 0x80 {
		return string([]byte{byte(codepoint)})
	}
	count, remaining := uint32(1), codepoint>>6
	for remaining >= 1<<(6-count) {
		count++
		remaining >>= 6
	}
	bytes := []byte{byte(0xff<<(7-count)) | byte(remaining)}
	for i := count; i > 0; i-- {
		bytes = append(bytes, byte(0x80|(codepoint>>((i-1)*6))&0x3f))
	}
	return string(bytes)
}

func (p *parser) stringValue() (string, bool) {
	if p.peek() != '[' {
		return p.quoted()
	}
	body, ok := p.longBody()
	if !ok {
		return "", false
	}
	// A leading newline is dropped and every newline sequence becomes LF.
	pairs := func(a, b byte) bool { return (a == '\r' && b == '\n') || (a == '\n' && b == '\r') }
	position := 0
	if position < len(body) && (body[0] == '\r' || body[0] == '\n') {
		position++
		if position < len(body) && pairs(body[0], body[position]) {
			position++
		}
	}
	var text strings.Builder
	for position < len(body) {
		b := body[position]
		position++
		if b == '\r' || b == '\n' {
			if position < len(body) && pairs(b, body[position]) {
				position++
			}
			b = '\n'
		}
		text.WriteByte(b)
	}
	return text.String(), true
}

func (p *parser) numeric(v *value) bool {
	start := p.position
	negative := p.peek() == '-'
	if negative {
		p.position++
	}
	hexadecimal := p.peek() == '0' && p.position+1 < len(p.source) &&
		(p.source[p.position+1] == 'x' || p.source[p.position+1] == 'X')
	if hexadecimal {
		p.position += 2
	}
	isNumberDigit := func(b byte) bool {
		if hexadecimal {
			return hexDigit(b) >= 0
		}
		return isDigit(b)
	}
	digits := 0
	for isNumberDigit(p.peek()) {
		p.position++
		digits++
	}
	integer := true
	if p.peek() == '.' {
		integer = false
		p.position++
		for isNumberDigit(p.peek()) {
			p.position++
			digits++
		}
	}
	if digits == 0 {
		return p.fail("invalid literal number")
	}
	exponent := false
	if (hexadecimal && (p.peek() == 'p' || p.peek() == 'P')) || (!hexadecimal && (p.peek() == 'e' || p.peek() == 'E')) {
		integer, exponent = false, true
		p.position++
		if p.peek() == '+' || p.peek() == '-' {
			p.position++
		}
		digitsStart := p.position
		for isDigit(p.peek()) {
			p.position++
		}
		if p.position == digitsStart {
			return p.fail("number exponent needs digits")
		}
	}
	token := p.source[start:p.position]
	if len(token) > 128 || isIdentifierPart(p.peek()) || p.peek() == '.' {
		return p.fail("unsupported literal number")
	}
	if integer {
		if hexadecimal {
			number, err := strconv.ParseUint(strings.TrimPrefix(token, "-")[2:], 16, 64)
			if err != nil {
				return p.fail("hex integer is out of range")
			}
			if negative {
				number = -number
			}
			v.kind, v.integer = integerKind, int64(number)
			return true
		}
		if number, err := strconv.ParseInt(token, 10, 64); err == nil {
			v.kind, v.integer = integerKind, number
			return true
		}
	}
	if hexadecimal && !exponent {
		// C strtod accepts hex floats without an exponent; Go needs one.
		token += "p0"
	}
	number, err := strconv.ParseFloat(token, 64)
	if err != nil || math.IsInf(number, 0) || math.IsNaN(number) {
		return p.fail("number is not finite or representable")
	}
	v.kind, v.number = numberKind, number
	return true
}

func (p *parser) keyword(word string) bool {
	if !strings.HasPrefix(p.source[p.position:], word) {
		return false
	}
	end := p.position + len(word)
	if end < len(p.source) && isIdentifierPart(p.source[end]) {
		return false
	}
	p.position = end
	return true
}

func (p *parser) atKeyword(word string) bool {
	start := p.position
	matched := p.keyword(word)
	p.position = start
	return matched
}

var reservedWords = map[string]bool{
	"and": true, "break": true, "do": true, "else": true, "elseif": true, "end": true,
	"false": true, "for": true, "function": true, "goto": true, "if": true, "in": true,
	"local": true, "nil": true, "not": true, "or": true, "repeat": true, "return": true,
	"then": true, "true": true, "until": true, "while": true,
}

// keyIdentity maps keys to a comparable value so that two keys are the same
// exactly when the C parser's same_key calls them equal: an integer-valued
// number is the integer, other numbers compare by value, and strings and
// booleans by content.
type keyIdentity struct {
	class   byte
	text    string
	integer int64
	bits    uint64
}

func identityOf(v *value) keyIdentity {
	switch v.kind {
	case integerKind:
		return keyIdentity{class: 'n', integer: v.integer}
	case numberKind:
		if v.number >= math.MinInt64 && v.number < -math.MinInt64 &&
			float64(int64(v.number)) == v.number {
			return keyIdentity{class: 'n', integer: int64(v.number)}
		}
		return keyIdentity{class: 'f', bits: math.Float64bits(v.number)}
	case booleanKind:
		if v.boolean {
			return keyIdentity{class: 'b', integer: 1}
		}
		return keyIdentity{class: 'b'}
	}
	return keyIdentity{class: 's', text: v.text}
}

func (p *parser) table(depth int) *value {
	if depth == literalDepthLimit {
		p.fail("literal table depth limit exceeded")
		return nil
	}
	v := p.newValue(tableKind)
	if v == nil {
		return nil
	}
	p.position++
	var arrayIndex int64
	seen := map[keyIdentity]struct{}{}
	if !p.skip() {
		return nil
	}
	for p.peek() != '}' {
		var e entry
		switch {
		case p.peek() == '[' && p.bracket(p.position, '[') == p.position:
			p.position++
			if !p.skip() {
				return nil
			}
			if e.key = p.parseValue(depth + 1); e.key == nil {
				return nil
			}
			if e.key.kind == tableKind {
				p.fail("table keys are unsupported")
				return nil
			}
			if !p.skip() || p.peek() != ']' {
				p.fail("explicit key needs closing bracket")
				return nil
			}
			p.position++
			if !p.skip() || p.peek() != '=' {
				p.fail("explicit key needs equals")
				return nil
			}
			p.position++
		case isIdentifierStart(p.peek()) && !p.atKeyword("true") && !p.atKeyword("false"):
			start := p.position
			for isIdentifierPart(p.peek()) {
				p.position++
			}
			name := p.source[start:p.position]
			if reservedWords[name] {
				p.fail("reserved word cannot be a field key")
				return nil
			}
			if e.key = p.newValue(stringKind); e.key == nil {
				return nil
			}
			e.key.text = name
			if !p.skip() || p.peek() != '=' {
				p.fail("literal field needs equals")
				return nil
			}
			p.position++
		default:
			if e.key = p.newValue(integerKind); e.key == nil {
				return nil
			}
			arrayIndex++
			e.key.integer, e.implicit = arrayIndex, true
		}
		if !p.skip() {
			return nil
		}
		if e.value = p.parseValue(depth + 1); e.value == nil {
			return nil
		}
		identity := identityOf(e.key)
		if _, duplicate := seen[identity]; duplicate {
			p.fail("duplicate literal table key")
			return nil
		}
		seen[identity] = struct{}{}
		v.entries = append(v.entries, e)
		if !p.skip() {
			return nil
		}
		if p.peek() == ',' || p.peek() == ';' {
			p.position++
			if !p.skip() {
				return nil
			}
		} else if p.peek() != '}' {
			p.fail("table entry needs separator")
			return nil
		}
	}
	p.position++
	return v
}

func (p *parser) parseValue(depth int) *value {
	switch {
	case p.peek() == '{':
		return p.table(depth)
	case p.peek() == '\'' || p.peek() == '"' || p.peek() == '[':
		v := p.newValue(stringKind)
		if v == nil {
			return nil
		}
		text, ok := p.stringValue()
		if !ok {
			return nil
		}
		v.text = text
		return v
	case isDigit(p.peek()) || p.peek() == '.' || p.peek() == '-':
		v := p.newValue(numberKind)
		if v == nil || !p.numeric(v) {
			return nil
		}
		return v
	case p.keyword("true"):
		v := p.newValue(booleanKind)
		if v != nil {
			v.boolean = true
		}
		return v
	case p.keyword("false"):
		return p.newValue(booleanKind)
	}
	p.fail("unsupported Lua expression; expected literal value")
	return nil
}

// parseLiteral parses a whole document; the error carries line and column.
func parseLiteral(source string) (*value, error) {
	p := &parser{source: source}
	var root *value
	if nul := strings.IndexByte(source, 0); nul >= 0 {
		p.position = nul
		p.fail("literal source contains NUL")
	} else if p.skip() && p.keyword("return") && p.skip() && p.peek() == '{' {
		root = p.parseValue(0)
		if root != nil && p.skip() {
			if p.peek() == ';' {
				p.position++
				p.skip()
			}
			if p.position != len(source) {
				p.fail("unexpected bytes after return table")
			}
		}
	} else {
		p.fail("literal configuration must return one table")
	}
	if p.failure == "" && root != nil {
		return root, nil
	}
	line, column := 1, 1
	for i := 0; i < p.failurePosition && i < len(source); i++ {
		if source[i] == '\r' || source[i] == '\n' {
			if i+1 < p.failurePosition && ((source[i] == '\r' && source[i+1] == '\n') ||
				(source[i] == '\n' && source[i+1] == '\r')) {
				i++
			}
			line++
			column = 1
		} else {
			column++
		}
	}
	return nil, &parseError{line, column, p.failure}
}

// field returns the value of a string-keyed entry.
func (v *value) field(key string) *value {
	if v == nil || v.kind != tableKind {
		return nil
	}
	for _, e := range v.entries {
		if e.key.kind == stringKind && e.key.text == key {
			return e.value
		}
	}
	return nil
}

func stringValue(text string) *value { return &value{kind: stringKind, text: text} }
func boolValue(b bool) *value        { return &value{kind: booleanKind, boolean: b} }
func integerValue(n int64) *value    { return &value{kind: integerKind, integer: n} }
func tableValue() *value             { return &value{kind: tableKind} }

func (v *value) set(key string, item *value) {
	v.entries = append(v.entries, entry{key: stringValue(key), value: item})
}

func (v *value) appendItem(item *value) {
	v.entries = append(v.entries, entry{key: integerValue(int64(len(v.entries) + 1)), value: item, implicit: true})
}

// writeLiteral formats a document as "return { ... }" with two-space indents.
func writeLiteral(root *value) string {
	var out strings.Builder
	out.WriteString("return ")
	writeValue(&out, root, 0)
	out.WriteString("\n")
	return out.String()
}

func writeValue(out *strings.Builder, v *value, depth int) {
	switch v.kind {
	case stringKind:
		out.WriteString(quote(v.text))
	case integerKind:
		out.WriteString(strconv.FormatInt(v.integer, 10))
	case numberKind:
		text := strconv.FormatFloat(v.number, 'g', -1, 64)
		if !strings.ContainsAny(text, ".e") {
			text += ".0"
		}
		out.WriteString(text)
	case booleanKind:
		out.WriteString(strconv.FormatBool(v.boolean))
	case tableKind:
		if len(v.entries) == 0 {
			out.WriteString("{}")
			return
		}
		if inlineList(v) {
			out.WriteString("{ ")
			for i, e := range v.entries {
				if i > 0 {
					out.WriteString(", ")
				}
				writeValue(out, e.value, depth+1)
			}
			out.WriteString(" }")
			return
		}
		indent := strings.Repeat("  ", depth+1)
		out.WriteString("{\n")
		for _, e := range v.entries {
			out.WriteString(indent)
			if !e.implicit {
				writeKey(out, e.key)
				out.WriteString(" = ")
			}
			writeValue(out, e.value, depth+1)
			out.WriteString(",\n")
		}
		out.WriteString(strings.Repeat("  ", depth))
		out.WriteString("}")
	}
}

// inlineList is true for a list of scalars, such as cpus = { 1, 2, 3 }.
func inlineList(v *value) bool {
	for _, e := range v.entries {
		if !e.implicit || e.value.kind == tableKind {
			return false
		}
	}
	return true
}

func writeKey(out *strings.Builder, key *value) {
	if key.kind == stringKind && identifier(key.text) {
		out.WriteString(key.text)
		return
	}
	out.WriteString("[")
	writeValue(out, key, 0)
	out.WriteString("]")
}

func identifier(text string) bool {
	if text == "" || !isIdentifierStart(text[0]) || reservedWords[text] {
		return false
	}
	for i := 1; i < len(text); i++ {
		if !isIdentifierPart(text[i]) {
			return false
		}
	}
	return true
}

func quote(text string) string {
	var out strings.Builder
	out.WriteByte('"')
	for i := 0; i < len(text); i++ {
		b := text[i]
		switch {
		case b == '"' || b == '\\':
			out.WriteByte('\\')
			out.WriteByte(b)
		case b == '\n':
			out.WriteString(`\n`)
		case b < 0x20 || b == 0x7f:
			fmt.Fprintf(&out, `\x%02x`, b)
		default:
			out.WriteByte(b)
		}
	}
	out.WriteByte('"')
	return out.String()
}
