module std::core::string::iterator;

struct StringIterator
{
	String utf8;
	usz current;
}

fn void StringIterator.reset(&self)
{
	self.current = 0;
}

fn Char32? StringIterator.next(&self)
{
    usz len = self.utf8.len;
    usz current = self.current;
    if (current >= len) return NO_MORE_ELEMENT?;
    usz read = (len - current < 4 ? len - current : 4);
    Char32 res = conv::utf8_to_char32(&self.utf8[current], &read)!;
    self.current += read;
    return res;
}

fn Char32? StringIterator.peek(&self)
{
    usz len = self.utf8.len;
    usz current = self.current;
    if (current >= len) return NO_MORE_ELEMENT?;
    usz read = (len - current < 4 ? len - current : 4);
    Char32 res = conv::utf8_to_char32(&self.utf8[current], &read)!;
    return res;
}

fn bool StringIterator.has_next(&self)
{
    return self.current < self.utf8.len;
}

fn Char32? StringIterator.get(&self)
{
    usz len = self.utf8.len;
    usz current = self.current;
    usz read = (len - current < 4 ? len - current : 4);
    usz index = current > read ? current - read : 0;
    if (index >= len) return NO_MORE_ELEMENT?;
    Char32 res = conv::utf8_to_char32(&self.utf8[index], &read)!;
    return res;
}
