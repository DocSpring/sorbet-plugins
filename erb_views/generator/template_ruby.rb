# frozen_string_literal: true

module ErbViews
  # A template's Ruby as the plugin gives it to Sorbet: `templateRuby` in ErbViews.cc. Every byte stays where it was:
  # text and comments become spaces (newlines kept), tag delimiters become spaces, and the closing delimiter's second
  # byte (the `>` of `%>`, the `%` of `-%>`) becomes `;`, where Herb puts it. Keep the two in step:
  # scripts/conformance.rb compares them.
  #
  # Two comments are code (`magicComment`): `<%# locals: (a:, b: 1) %>` becomes `__locals=->(a:, b: 1){};` and
  # `<%# sig { params(a: Integer) } %>` becomes `sig { params(a: Integer) };`. With `magic: false` they stay comments,
  # as Herb reads them.
  module TemplateRuby
    OPEN = '<%'.b
    CLOSE = '%>'.b
    LAMBDA = '__locals=->'.b
    BLANKS = [' ', "\t", "\n", "\r"].map(&:ord).freeze

    def self.call(source, magic: true)
      source = source.b
      out = blank(source)
      i = 0
      while i < source.bytesize
        open = source.index(OPEN, i)
        break if open.nil?

        close = source.index(CLOSE, open + 2)
        break if close.nil? # Erubi leaves an unclosed tag as text.

        tag_end = close + 2
        code_start = open + 2
        indicator = code_start < close ? source.byteslice(code_start, 1) : ''
        if indicator == '#' && magic && magic_comment(out, source, open, close, tag_end)
          i = tag_end
          next
        end
        if ['%', '#'].include?(indicator) # literal text, comment
          i = tag_end
          next
        end
        if indicator == '='
          code_start += 1
          code_start += 1 if code_start < close && source.byteslice(code_start, 1) == '='
        elsif indicator == '-'
          code_start += 1
        end
        code_end = close
        code_end -= 1 if code_end > code_start && ['-', '='].include?(source.byteslice(code_end - 1, 1))
        out[code_start...code_end] = source.byteslice(code_start, code_end - code_start)
        out[code_end + 1] = ';'
        i = tag_end
      end
      out.force_encoding(Encoding::UTF_8)
    end

    # Writes a magic comment's code into `out` (already blank there) and returns true, or returns false for any other
    # comment. `close` is where the tag's `%>` starts.
    def self.magic_comment(out, source, open, close, tag_end)
      at = open + 3
      at += 1 while at < close && BLANKS.include?(source.getbyte(at))
      return false if at == open + 3

      text = source.byteslice(at, close - at)
      code_end = close > at && source.getbyte(close - 1) == '-'.ord ? close - 1 : close
      if text.start_with?('locals:')
        paren = at + 7
        return false if paren >= close || !BLANKS.include?(source.getbyte(paren))

        paren += 1 while paren < close && BLANKS.include?(source.getbyte(paren))
        last = code_end
        last -= 1 while last > paren && BLANKS.include?(source.getbyte(last - 1))
        # The lambda's arrow goes before the `(` and its `{}` after the `)`, where no newline may be overwritten.
        return false if paren >= close || source.getbyte(paren) != '('.ord || last <= paren + 1 || source.getbyte(last - 1) != ')'.ord ||
                        paren - open < LAMBDA.bytesize || tag_end - last < 2 ||
                        source.byteslice(paren - LAMBDA.bytesize, LAMBDA.bytesize).include?("\n") ||
                        source.byteslice(last, 2).include?("\n")

        out[(paren - LAMBDA.bytesize)...paren] = LAMBDA
        out[paren...last] = source.byteslice(paren...last)
        out[last, 2] = '{}'
        out[last + 2] = ';' if tag_end - last >= 3 && source.getbyte(last + 2) != "\n".ord
        return true
      end
      return false unless text.start_with?('sig ', 'sig{')

      out[at...code_end] = source.byteslice(at...code_end)
      out[code_end + 1] = ';'
      true
    end

    # Spaces in place of every byte but newlines.
    def self.blank(source) = source.gsub(/[^\n]/n, ' ')
  end
end
