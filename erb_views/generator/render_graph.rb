# frozen_string_literal: true

require_relative 'app'
require_relative 'code'
require_relative 'template_ruby'

module ErbViews
  # Which templates each action renders, and what reaches each template: the renderers (controllers and mailers)
  # that render it, the instance variables they and the templates above it assign, and the locals its callers pass.
  class RenderGraph
    # An instance variable's assignments that reach a template, and whether one always runs first.
    Ivar = Struct.new(:writes, :definite, keyword_init: true)
    # A place that assigns an ivar: its file (relative to the app), its Prism node, and the value's node.
    Write = Struct.new(:file, :node, :value, keyword_init: true) do
      def key = [file, node.location.start_offset]
    end
    # A literal render a template makes of a partial, with the locals it passes (name => value node, or nil when
    # the value isn't in the call, as with `collection:`'s local).
    PartialCall = Struct.new(:file, :node, :partial, :locals, keyword_init: true)
    # Not an ivar: an env entry that's definite when `authenticate_user!` ran.
    AUTHENTICATED = '!authenticated'
    # What reaches one template (a logical name: all its formats).
    Context = Struct.new(:renderers, :ivars, :calls, keyword_init: true) do
      # Whether every render of the template comes after an `authenticate_user!` that always runs.
      def authenticated? = ivars[AUTHENTICATED]&.definite || false
    end

    # Render options that don't render a template.
    NOT_TEMPLATES = %i[plain json html body inline file js xml nothing text].freeze

    attr_reader :app, :contexts, :dynamic_renders

    def initialize(app)
      @app = app
      @contexts = Hash.new { |hash, logical| hash[logical] = Context.new(renderers: Set.new, ivars: {}, calls: []) }
      @template_facts = {}
      @method_cache = {}
      @dynamic_renders = []
      build
    end

    # A template's own code: its writes and renders.
    def template_facts(file)
      @template_facts[file] ||= Code.scan(Prism.parse(TemplateRuby.call(File.read(File.join(app.root, file)))).value, file)
    end

    private

    def build
      queue = []
      app.renderers.each do |renderer|
        renderer.actions.each { |action| queue.concat(action_visits(renderer, action)) }
        renderer.klass.rescue_handlers.each do |_exception, handler|
          next unless handler.is_a?(Symbol)

          facts = collect(renderer, handler, Set.new)
          queue.concat(render_visits(renderer, facts.renders, writes_env({}, facts), default: nil))
        end
      end
      # An ActiveAdmin registration's `show`, `panel` and `sidebar` blocks are views of its controller.
      app.active_admin_blocks.each do |renderer, facts|
        queue.concat(view_visits(renderer, facts, {}))
      end
      # Propagate down the templates until nothing changes: each visit merges into its template's context, and a
      # context that changed passes its ivars on to its partials and layouts.
      until queue.empty?
        logical, renderer, env, layouts = queue.shift
        context = @contexts[logical]
        new_renderer = !context.renderers.add?(renderer.name).nil?
        merged = merge_into(context.ivars, env, first: context.renderers.size == 1 && new_renderer)
        changed = new_renderer || merged
        next unless changed || layouts

        app.files(logical).each do |file|
          facts = template_facts(file)
          below = writes_env(context.ivars, facts)
          queue.concat(view_visits(renderer, facts, below))
          (layouts || []).each { |layout| queue << [layout, renderer, below, nil] }
        end
      end
    end

    # The visits one action makes: its explicit renders and, unless it always responds some other way, its
    # default template.
    def action_visits(renderer, action)
      env = {}
      visits = []
      app.callbacks(renderer, action).each do |callback|
        env = env.merge(AUTHENTICATED => Ivar.new(writes: [], definite: true)) if callback.method == 'authenticate_user!' && callback.always
        facts = expand(renderer, callback.facts, Set.new)
        facts = conditional(facts) unless callback.always
        env = writes_env(env, facts)
        visits.concat(render_visits(renderer, facts.renders, env, default: action))
        # A callback that always renders or redirects stops the chain.
        return visits if responds?(facts)
      end
      facts = collect(renderer, action.to_sym, Set.new)
      env = writes_env(env, facts)
      visits.concat(render_visits(renderer, facts.renders, env, default: action))
      unless responds?(facts)
        logical = app.resolve(action, renderer.prefixes, partial: false)
        visits << [logical, renderer, env, renderer.layouts] if logical
      end
      visits
    end

    def responds?(facts) = facts.renders.any?(&:definite) || facts.responds.any?(&:definite)

    # A method's facts, with the writes and renders of the renderer's own methods it calls.
    def collect(renderer, name, seen)
      collect_method(renderer, renderer.klass.instance_method(name), seen)
    rescue NameError
      Code::Facts.empty
    end

    # `call` (a Code::SelfCall) binds the method's parameters for its renders.
    def collect_method(renderer, method, seen, call = nil)
      key = [method.owner, method.name]
      return Code::Facts.empty unless seen.add?(key)

      facts = @method_cache[key] ||= app.method_facts(method) || Code::Facts.empty
      facts = bind(facts, method, call) if call && facts.renders.any?
      expand(renderer, facts, seen, method)
    end

    def bind(facts, method, call)
      _body, definition, _file = app.unbound_body(method)
      return facts unless definition.is_a?(Prism::DefNode) && definition.parameters

      parameters = definition.parameters
      arguments = call.node.arguments&.arguments || []
      keywords = arguments.find { |argument| argument.is_a?(Prism::KeywordHashNode) }
      positional = arguments - [keywords]
      bindings = {}
      (parameters.requireds + parameters.optionals).each_with_index do |parameter, index|
        bindings[parameter.name] = [positional[index], call.scope] if parameter.respond_to?(:name) && positional[index]
      end
      keyword_names = parameters.keywords.map(&:name)
      (keywords&.elements || []).each do |element|
        key = element.is_a?(Prism::AssocNode) && Code.literal(element.key)
        bindings[key.to_sym] = [element.value, call.scope] if key && keyword_names.include?(key.to_sym)
      end
      renders = facts.renders.map { |render| render.dup.tap { |copy| copy.bindings = bindings } }
      Code::Facts.new(writes: facts.writes, calls: facts.calls, renders:, responds: facts.responds, local_keys: facts.local_keys)
    end

    # `method` is the method `facts` came from, which a `super` in it needs.
    def expand(renderer, facts, seen, method = nil)
      result = Code::Facts.new(writes: facts.writes.dup, calls: [], renders: facts.renders.dup, responds: facts.responds.dup, local_keys: [])
      facts.calls.each do |call|
        if call.name == Code::SUPER
          parent = method&.super_method
          next unless parent && app.follow_method?(parent)

          called = collect_method(renderer, parent, seen)
        else
          next unless app.follow?(renderer.klass, call.name)

          called = begin
            collect_method(renderer, renderer.klass.instance_method(call.name), seen, call)
          rescue NameError
            Code::Facts.empty
          end
        end
        called = conditional(called) unless call.definite
        result.writes.concat(called.writes)
        result.renders.concat(called.renders)
        result.responds.concat(called.responds)
      end
      result
    end

    def conditional(facts)
      Code::Facts.new(
        writes: facts.writes.map { |w| w.dup.tap { |c| c.definite = false } },
        calls: facts.calls,
        renders: facts.renders.map { |r| r.dup.tap { |c| c.definite = false } },
        responds: facts.responds.map { |r| r.dup.tap { |c| c.definite = false } },
        local_keys: facts.local_keys,
      )
    end

    # `env` with `facts`' writes added. A definite write makes the ivar definite.
    def writes_env(env, facts)
      result = env.transform_values { |ivar| Ivar.new(writes: ivar.writes.dup, definite: ivar.definite) }
      facts.writes.each do |write|
        ivar = (result[write.name] ||= Ivar.new(writes: [], definite: false))
        added = Write.new(file: write.file, node: write.node, value: write.value)
        ivar.writes << added unless ivar.writes.any? { |w| w.key == added.key }
        ivar.definite ||= write.definite
      end
      result
    end

    # Merges `env` into a context's ivars: the union of writes, and definite only when definite in every visit.
    def merge_into(ivars, env, first:)
      changed = false
      if first
        env.each do |name, ivar|
          ivars[name] = Ivar.new(writes: ivar.writes.dup, definite: ivar.definite)
          changed = true
        end
        return changed
      end
      (ivars.keys | env.keys).each do |name|
        mine = ivars[name]
        theirs = env[name]
        if mine.nil?
          ivars[name] = Ivar.new(writes: theirs.writes.dup, definite: false)
          changed = true
        elsif theirs.nil?
          changed ||= mine.definite
          mine.definite = false
        else
          before = [mine.writes.size, mine.definite]
          theirs.writes.each { |write| mine.writes << write unless mine.writes.any? { |w| w.key == write.key } }
          mine.definite &&= theirs.definite
          changed ||= before != [mine.writes.size, mine.definite]
        end
      end
      changed
    end

    # A controller's or mailer's render calls, as templates to visit.
    def render_visits(renderer, renders, env, default:)
      renders.flat_map do |render|
        first, options = Code.render_arguments(render.node)
        next [] if (options.keys & NOT_TEMPLATES).any?

        if options.key?(:partial)
          logicals = partials(renderer, renderer.klass, options[:partial], render.scope, render.bindings)
          next record_dynamic(render) if logicals.empty?

          logicals.map do |logical|
            record_call(render, logical, call_locals(render, nil, options, logical))
            [logical, renderer, env, nil]
          end
        else
          node = first || options[:template] || options[:action]
          names = node ? names(renderer.klass, node, render.scope, render.bindings) : [default].compact
          next [] if names.nil? # render SomeComponent.new(...), render @record

          templates = names.flat_map { |name| known?(name) ? app.resolve_all(name, renderer.prefixes, partial: false) : [] }
          next record_dynamic(render) if templates.empty?

          layouts = layout_option(renderer, options)
          templates.map do |template|
            record_call(render, template, call_locals(render, nil, options, template)) if options.key?(:locals)
            [template, renderer, env, layouts]
          end
        end
      end
    end

    def layout_option(renderer, options)
      return renderer.layouts unless options.key?(:layout)

      value = options[:layout]
      return [] if value.is_a?(Prism::FalseNode)

      name = Code.literal(value)
      return renderer.layouts if name.nil?

      logical = App.layout_name(name)
      app.files(logical).empty? ? [] : [logical]
    end

    # The renders in a view's code: its `render` calls, and its calls to helpers that render a partial they're given.
    def view_visits(renderer, facts, env)
      facts.renders.flat_map { |render| view_render(renderer, render, env) } +
        facts.calls.flat_map { |call| wrapper_render(renderer, call, env) }
    end

    # A view's render call: a partial (or a template, with `template:`), with the locals it passes. After a name
    # (`render 'row', html: true`), the hash is the partial's locals, not options.
    def view_render(renderer, render, env)
      first, options = Code.render_arguments(render.node)
      return [] if first.nil? && (options.keys & NOT_TEMPLATES).any?

      if first.nil? && options.key?(:template)
        names = names(renderer.klass._helpers, options[:template], render.scope) || []
        return names.flat_map { |name| known?(name) ? app.resolve_all(name, [], partial: false) : [] }
                    .map { |logical| [logical, renderer, env, nil] }
      end
      node = first || options[:partial] || options[:layout]
      return [] if node.nil?

      logicals = partials(renderer, renderer.klass._helpers, node, render.scope)
      # `render @record` or `render SomeComponent.new(...)` isn't a partial we can name; a string we can't follow is.
      return string?(node) || options.key?(:partial) ? record_dynamic(render) : [] if logicals.empty?

      logicals.map do |logical|
        record_call(render, logical, call_locals(render, first, options, logical))
        [logical, renderer, env, nil]
      end
    end

    # A call to a helper that renders the partial it's given (`render_code_example 'x/example', locals: {...}`).
    def wrapper_render(renderer, call, env)
      wrapper = app.render_wrapper(renderer.klass._helpers, call.name)
      return [] if wrapper.nil?

      arguments = call.node.arguments&.arguments || []
      node = arguments[wrapper.partial]
      return record_dynamic(call) if node.nil?

      logicals = partials(renderer, renderer.klass._helpers, node, call.scope)
      return record_dynamic(call) if logicals.empty?

      locals = wrapper.locals && wrapper_locals(arguments[wrapper.locals[0]], wrapper.locals[1])
      logicals.map do |logical|
        record_call(call, logical, hash_locals(locals))
        [logical, renderer, env, nil]
      end
    end

    def wrapper_locals(argument, key)
      return argument if key.nil?
      return nil unless argument.is_a?(Prism::HashNode) || argument.is_a?(Prism::KeywordHashNode)

      element = argument.elements.find { |e| e.is_a?(Prism::AssocNode) && Code.literal(e.key) == key.to_s }
      element&.value
    end

    # The partials a render name can be: a literal, an interpolation (`"template_settings/#{tab}"` matches every
    # partial in template_settings), a local or a helper that returns one of a few names.
    def partials(renderer, owner, node, scope, bindings = nil)
      names = names(owner, node, scope, bindings) || []
      names.flat_map { |name| known?(name) ? app.resolve_all(name, renderer.prefixes, partial: true) : [] }.uniq
    end

    def names(owner, node, scope, bindings = nil)
      Code.string_values(node, scope, 0, bindings) do |name|
        body, definition, file = app.method_body(owner, name)
        [body, definition] if body && app.app_file?(file)
      end
    end

    # A name with nothing known but its unknown parts would match every partial.
    def known?(name) = !name.delete(Code::UNKNOWN).delete('/').empty?

    def string?(node) = node.is_a?(Prism::StringNode) || node.is_a?(Prism::InterpolatedStringNode) || node.is_a?(Prism::SymbolNode)

    # The locals a partial render passes: `render 'x', a: 1`, `render partial: 'x', locals: { a: 1 }`, and the local
    # that `collection:` or `object:` sets (the partial's name, or `as:`).
    def call_locals(render, first, options, logical)
      # `render 'row', a: 1` in a view: the whole hash is locals, with the keys of any hash it splats.
      if first
        splats = (render.node.arguments&.arguments || []).grep(Prism::KeywordHashNode).flat_map(&:elements).grep(Prism::AssocSplatNode)
        splatted = splats.flat_map { |splat| Code.hash_keys(splat.value, render.scope) }
        return splatted.to_h { |key| [key, nil] }.merge(options.to_h { |key, value| [key.to_s, value] })
      end

      locals = hash_locals(options[:locals])
      if options.key?(:collection) || options.key?(:object)
        as = Code.literal(options[:as]) || File.basename(logical).delete_prefix('_')
        locals[as] = nil
        if options.key?(:collection)
          locals["#{as}_counter"] = nil
          locals["#{as}_iteration"] = nil
        end
      end
      locals.except('partial', 'locals', 'collection', 'object', 'as', 'layout', 'cached', 'spacer_template', 'formats')
    end

    def record_call(render, logical, locals)
      context = @contexts[logical]
      return if context.calls.any? { |c| c.file == render.file && c.node.equal?(render.node) }

      context.calls << PartialCall.new(file: render.file, node: render.node, partial: logical, locals:)
    end

    def hash_locals(node)
      return {} unless node.is_a?(Prism::HashNode) || node.is_a?(Prism::KeywordHashNode)

      node.elements.each_with_object({}) do |element, locals|
        key = element.is_a?(Prism::AssocNode) && Code.literal(element.key)
        locals[key] = element.value if key
      end
    end

    # A render the generator can't follow. Returns no visits.
    def record_dynamic(render)
      @dynamic_renders << [render.file, render.node.location.start_line, render.node.slice[0, 120]]
      []
    end
  end
end
