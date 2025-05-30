module std::io;
import std::math;

struct ByteBuffer (InStream, OutStream)
{
	Allocator allocator;
	usz max_read;
	char[] bytes;
	usz read_idx;
	usz write_idx;
	bool has_last;
}

<*
 ByteBuffer provides a streamable read/write buffer.
 max_read defines how many bytes might be kept before its internal buffer is shrinked.
 @require self.bytes.len == 0 : "Buffer already initialized."
*>
fn ByteBuffer* ByteBuffer.init(&self, Allocator allocator, usz max_read, usz initial_capacity = 16)
{
	*self = { .allocator = allocator, .max_read = max_read };
	initial_capacity = max(initial_capacity, 16);
	self.grow(initial_capacity);
	return self;
}

fn ByteBuffer* ByteBuffer.tinit(&self, usz max_read, usz initial_capacity = 16)
{
	return self.init(tmem, max_read, initial_capacity);
}

<*
 @require buf.len > 0
 @require self.bytes.len == 0 : "Buffer already initialized."
*>
fn ByteBuffer* ByteBuffer.init_with_buffer(&self, char[] buf)
{
	*self = { .max_read = buf.len, .bytes = buf };
	return self;
}

fn void ByteBuffer.free(&self)
{
	if (self.allocator) allocator::free(self.allocator, self.bytes);
	*self = {};
}

fn usz? ByteBuffer.write(&self, char[] bytes) @dynamic
{
	usz cap = self.bytes.len - self.write_idx;
	if (cap < bytes.len) self.grow(bytes.len);
	self.bytes[self.write_idx:bytes.len] = bytes[..];
	self.write_idx += bytes.len;
	return bytes.len;
}

fn void? ByteBuffer.write_byte(&self, char c) @dynamic
{
	usz cap = self.bytes.len - self.write_idx;
	if (cap == 0) self.grow(1);
	self.bytes[self.write_idx] = c;
	self.write_idx++;
}

fn usz? ByteBuffer.read(&self, char[] bytes) @dynamic
{
	usz readable = self.write_idx - self.read_idx;
	if (readable == 0)
	{
		self.has_last = false;
		return io::EOF?;
	}
	usz n = min(readable, bytes.len);
	bytes[:n] = self.bytes[self.read_idx:n];
	self.read_idx += n;
	self.has_last = n > 0;
	self.shrink();
	return n;
}

fn char? ByteBuffer.read_byte(&self) @dynamic
{
	usz readable = self.write_idx - self.read_idx;
	if (readable == 0)
	{
		self.has_last = false;
		return io::EOF?;
	}
	char c = self.bytes[self.read_idx];
	self.read_idx++;
	self.has_last = true;
	self.shrink();
	return c;
}

<*
 Only the last byte of a successful read can be pushed back.
*>
fn void? ByteBuffer.pushback_byte(&self) @dynamic
{
	if (!self.has_last) return io::EOF?;
	assert(self.read_idx > 0);
	self.read_idx--;
	self.has_last = false;
}

fn usz? ByteBuffer.seek(&self, isz offset, Seek seek) @dynamic
{
	switch (seek)
	{
		case SET:
			if (offset < 0 || offset > self.write_idx) return INVALID_POSITION?;
			self.read_idx = offset;
			return offset;
		case CURSOR:
			if ((offset < 0 && self.read_idx < -offset) ||
				(offset > 0 && self.read_idx + offset > self.write_idx)) return INVALID_POSITION?;
			self.read_idx += offset;
		case END:
			if (offset < 0 || offset > self.write_idx) return INVALID_POSITION?;
			self.read_idx = self.write_idx - offset;
	}
	return self.read_idx;
}

fn usz? ByteBuffer.available(&self) @inline @dynamic
{
	return self.write_idx - self.read_idx;
}

fn void ByteBuffer.grow(&self, usz n)
{
	n = math::next_power_of_2(n);
	char* p = allocator::realloc(self.allocator, self.bytes, n);
	self.bytes = p[:n];
}

macro ByteBuffer.shrink(&self)
{
	if (self.read_idx >= self.max_read)
	{
		// Drop the read data besides the last byte (for pushback_byte).
		usz readable = self.write_idx - self.read_idx;
		self.bytes[:1 + readable] = self.bytes[self.read_idx - 1:1 + readable];
		self.write_idx = 1 + readable;
		self.read_idx = 1;
	}
}