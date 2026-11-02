# typed: true

# What generate_rbi.rb writes for the fixtures' templates, by hand: the shared contexts (in an app,
# sorbet/rbi/erb_views/contexts.rbi) and each template's own RBI (sorbet/rbi/erb_views/app/views/...).

class Template
  sig { returns(String) }
  def name; end
end

# A controller's view helpers (in an app, ApplicationController::HelperProxy).
class ControllerHelpers
  sig { params(text: String, url: String).returns(String) }
  def link_to(text, url); end

  # A controller's `helper_method`.
  sig { returns(Template) }
  def current_template; end

  sig { params(args: T.untyped, kwargs: T.untyped, blk: T.untyped).returns(T.untyped) }
  def render(*args, **kwargs, &blk); end
end

# A mailer's view helpers (in an app, ActionView::Base with ApplicationMailer::HelperMethods).
class MailerHelpers
  sig { params(name: String).returns(String) }
  def greeting(name); end

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

module SorbetViews
  # The partials' methods, which each partial's own file defines.
  module Partials; end

  # The classes the plugin's template classes inherit.
  module Contexts
    class Controller < ::ControllerHelpers
      include ::SorbetViews::Partials
    end

    class Mailer < ::MailerHelpers
      include ::SorbetViews::Partials
    end

    module Components
      module Ds
        class CardComponentHtml < ::Ds::CardComponent; end
      end
    end
  end

  # Each template's own RBI reopens its class: the instance variables it reads or writes, and the locals of a partial
  # without a locals comment.
  module Templates
    class ShowHtml
      def initialize
        @template = T.let(T.unsafe(nil), Template)
      end
    end

    class CardsHtml
      def initialize
        @template = T.let(T.unsafe(nil), Template)
      end
    end

    class AssignsHtml
      def initialize
        @page_title = T.let(nil, T.nilable(String))
      end
    end

    class RowPartialHtml
      sig { returns(Template) }
      def template; end
    end
  end

  module Layouts
    class ApplicationHtml
      def initialize
        @page_title = T.let(nil, T.nilable(String))
      end
    end
  end
end
