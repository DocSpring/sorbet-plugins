# frozen_string_literal: true

module ErbViews
  # A template's Ruby as the plugin gives it to Sorbet: `templateRuby` in ErbViews.cc. Every byte stays where it was:
  # text and comments become spaces (newlines kept), tag delimiters become spaces, and the closing delimiter's second
  # byte (the `>` of `%>`, the `%` of `-%>`) becomes `;`, where Herb puts it. Keep the two in step:
  # scripts/conformance.rb compares them.
  module TemplateRuby
    OPEN = '<%'.b
    CLOSE = '%>'.b

    def self.call(source)
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

    # Spaces in place of every byte but newlines.
    def self.blank(source) = source.gsub(/[^\n]/n, ' ')
  end
end
