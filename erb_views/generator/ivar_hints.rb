# frozen_string_literal: true

module ErbViews
  # A template's instance variable declarations, for what the generator can't infer (an ivar a gem or
  # `instance_variable_set` assigns, a value Sorbet can't type) or knows only loosely:
  #
  #   <%# ivars: (@template: Template, @api_token: T.nilable(ApiToken)) %>
  #
  # Each type, in Sorbet's syntax, replaces the inferred one in the template's context. The plugin leaves the comment
  # alone: the generated RBI declares the ivars.
  module IvarHints
    COMMENT = /<%#\s+ivars:\s+\((.*?)\)\s*-?%>/m
    ENTRY = /\A\s*(@[a-zA-Z_]\w*)\s*:\s+(\S.*?)\s*\z/m

    module_function

    # name => type text, from every ivars comment in the source. Raises for an entry it can't read.
    def read(source, file)
      source.scan(COMMENT).flatten.each_with_object({}) do |list, hints|
        split(list).each do |entry|
          match = ENTRY.match(entry) or raise ArgumentError, "#{file}: can't read #{entry.strip.inspect} in its ivars comment"
          raise ArgumentError, "#{file}: its ivars comments declare #{match[1]} twice" if hints.key?(match[1])

          hints[match[1]] = match[2].gsub(/\s+/, ' ')
        end
      end
    end

    # The entries of a list, split at the commas outside brackets.
    def split(list)
      entries = [+'']
      depth = 0
      list.each_char do |char|
        depth += 1 if '([{'.include?(char)
        depth -= 1 if ')]}'.include?(char)
        if char == ',' && depth.zero?
          entries << +''
        else
          entries.last << char
        end
      end
      entries.reject { |entry| entry.strip.empty? }
    end
  end
end
