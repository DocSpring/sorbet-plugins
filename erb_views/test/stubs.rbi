# typed: true

# What generate_rbi.rb writes for the fixtures' templates, by hand.

class Template
  sig { returns(String) }
  def name; end
end

class ViewBase
  sig { params(text: String, url: String).returns(String) }
  def link_to(text, url); end

  sig { params(args: T.untyped, kwargs: T.untyped, blk: T.untyped).returns(T.untyped) }
  def render(*args, **kwargs, &blk); end
end

module Ds
  class CardComponent
    sig { params(title: String).void }
    def initialize(title:)
      @title = T.let(title, String)
    end

    sig { returns(String) }
    def title; end
  end
end

# The templates' contexts. The plugin's classes (SorbetViews::Templates::ShowHtml) inherit them.
module SorbetViews
  module Contexts
    module Templates
      class ShowHtml < ViewBase
        def initialize
          @template = T.let(T.unsafe(nil), Template)
        end
      end

      class RowPartialHtml < ViewBase
        sig { returns(Template) }
        def template; end
      end

      class LooseHtml < ViewBase; end

      class CardPartialHtml < ViewBase; end
      class NoLocalsPartialHtml < ViewBase; end
      class AnyLocalsPartialHtml < ViewBase; end
      class TwicePartialHtml < ViewBase; end

      # What the generator declares for each partial a view renders by name: the partial's locals.
      class CardsHtml < ViewBase
        def initialize
          @template = T.let(T.unsafe(nil), Template)
        end

        sig { params(template: Template, title: T.untyped, count: Integer).returns(T.untyped) }
        def __partial__templates__card(template:, title: T.unsafe(nil), count: T.unsafe(nil)); end

        sig { params(template: Template, title: T.untyped, count: Integer).returns(T.untyped) }
        def __partial__card(template:, title: T.unsafe(nil), count: T.unsafe(nil)); end
      end

      class AssignsHtml < ViewBase
        def initialize
          @page_title = T.let(nil, T.nilable(String))
        end
      end
    end

    module Layouts
      class ApplicationHtml < ViewBase
        def initialize
          @page_title = T.let(nil, T.nilable(String))
        end
      end
    end

    module Errors
      class N404Html < ViewBase; end
    end

    module Components
      module Ds
        class CardComponentHtml < ::Ds::CardComponent; end
      end
    end
  end
end
