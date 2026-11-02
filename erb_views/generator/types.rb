# frozen_string_literal: true

module ErbViews
  # The types of the instance variables and locals that reach each template.
  class Types
    # A constant at the start of a type's name: not after `::`, `.` or a word character, and not a literal's text.
    CONSTANT = /(?<![\w:.'"])([A-Z]\w*(?:::[A-Z]\w*)*)/

    # A type with every constant from the root (`T.nilable(User)` => `T.nilable(::User)`): the contexts live in
    # SorbetViews, where `Templates::Row` could resolve to a view class instead of the app's.
    def self.qualify(type) = type.gsub(CONSTANT) { |constant| constant == 'T' || constant.start_with?('T::') ? constant : "::#{constant}" }

    def initialize(app, graph)
      @app = app
      @graph = graph
    end

    def ivar_type(_logical, _name, _ivar) = 'T.untyped'

    def local_type(_logical, _name) = 'T.untyped'
  end
end
