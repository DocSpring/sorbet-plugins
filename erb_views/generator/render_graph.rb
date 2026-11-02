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
    # A literal render a template makes of a partial, with the locals it passes: name => the value's node, nil when
    # the value isn't in the call (a key of a hash the call splats), or MAYBE when the hash gets the key only by index
    # (`options[:label] = x if y`), which may not run.
    PartialCall = Struct.new(:file, :node, :partial, :locals, keyword_init: true)
    MAYBE = :maybe
    # A local whose value isn't a node in the call: an element of `collection:` (its node), or a fixed type.
    Element = Struct.new(:node)
    Known = Struct.new(:type)
    # Not an ivar: an env entry that's definite when `authenticate_user!` ran.
    AUTHENTICATED = '!authenticated'
    # Where a template's ivars pass to the next one: a render call (`:wrap`, its node), the end of an action's method
    # (`:after`, the def or block) for its default template, or the end of a template (`:end`) for its layout. The
    # type probe reveals each ivar there, where Sorbet's flow typing knows the most (after `return unless @record`).
    # `file` is relative to the app, or absolute in a gem. A render in a method the action calls (`render_page(:show)`)
    # has the call as its `outer` handoff: there Sorbet knows the action's ivars (`return unless @record` before the
    # call), and at the render only those the method assigns (`inner`).
    #
    # A handoff in an action has its `callbacks`: [the end of a callback's method (an `:after` Handoff), the ivars it
    # reads or assigns], in the order they run, and `own_writes`, the ivars the action assigns. Sorbet types an ivar
    # in the action as the class declares it, so one only a callback sets (`@pack`, nil when `prepare_purchase`
    # redirects) has the type it has at the end of the last callback that uses it.
    #
    # An `:after` handoff's `exits` are the method's `return`s that don't end the response: its default template
    # renders there too, or a callback's chain carries on from there.
    Handoff = Struct.new(:file, :node, :kind, :outer, :inner, :callbacks, :own_writes, :exits) do
      def key = [file, kind, node && [node.location.start_offset, node.location.end_offset]]
      def origin_key = [key, outer&.key, callbacks&.map { |handoff, _| handoff.key }]
    end
    # One way into a template: the renderer, the ivars on the way, the layouts it renders in (nil for a partial) and
    # where it came from.
    Visit = Struct.new(:logical, :renderer, :env, :layouts, :origin, keyword_init: true)
    # What reaches one template (a logical name: all its formats). `origins`: handoff key => [Handoff, the ivars
    # that arrive from there].
    Context = Struct.new(:renderers, :ivars, :calls, :origins, keyword_init: true) do
      # Whether every render of the template comes after an `authenticate_user!` that always runs.
      def authenticated? = ivars[AUTHENTICATED]&.definite || false
    end

    # Render options that don't render a template.
    NOT_TEMPLATES = %i[plain json html body inline file js xml nothing text].freeze

    # For the type probe: `renderer_ivars`, renderer name => ivar => the writes any of its actions makes,
    # `callback_envs`, renderer name => the ivars after each action's callbacks, and `ending_calls`, [file, offset] =>
    # a call in an action's or callback's body that ends the response (`redirect_back_with(...) unless @pack`). The
    # probe puts `return` before each one, so Sorbet's flow typing after it knows it didn't run.
    attr_reader :app, :contexts, :dynamic_renders, :renderer_ivars, :callback_envs, :ending_calls

    def initialize(app)
      @app = app
      @contexts = Hash.new { |hash, logical| hash[logical] = Context.new(renderers: Set.new, ivars: {}, calls: [], origins: {}) }
      @template_facts = {}
      @method_cache = {}
      @outcomes = {}
      @dynamic_renders = []
      @renderer_ivars = Hash.new { |hash, name| hash[name] = Hash.new { |ivars, ivar| ivars[ivar] = [] } }
      @callback_envs = Hash.new { |hash, name| hash[name] = [] }
      @ending_calls = {}
      build
    end

    # A template's own code: its writes and renders.
    def template_facts(file)
      @template_facts[file] ||= Code.scan(Prism.parse(TemplateRuby.call(File.read(File.join(app.root, file)))).value, file)
    end

    private

    def build
      record_ruby_renders
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
        visit = queue.shift
        renderer = visit.renderer
        context = @contexts[visit.logical]
        new_renderer = !context.renderers.add?(renderer.name).nil?
        merged = merge_into(context.ivars, visit.env, first: context.renderers.size == 1 && new_renderer)
        record_origin(context, visit)
        changed = new_renderer || merged
        next unless changed || visit.layouts

        app.files(visit.logical).each do |file|
          facts = template_facts(file)
          below = writes_env(context.ivars, facts)
          queue.concat(view_visits(renderer, facts, below))
          (visit.layouts || []).each do |layout|
            queue << Visit.new(logical: layout, renderer:, env: below, layouts: nil, origin: Handoff.new(file, nil, :end))
          end
        end
      end
    end

    def record_origin(context, visit)
      key = visit.origin ? visit.origin.origin_key : [:unknown, visit.renderer.name]
      entry = context.origins[key]
      if entry
        merge_into(entry[1], visit.env, first: false)
      else
        context.origins[key] = [visit.origin, {}].tap { |(_, env)| merge_into(env, visit.env, first: true) }
      end
    end

    # The visits one action makes: its explicit renders and, unless it always responds some other way, its
    # default template.
    def action_visits(renderer, action)
      env = {}
      visits = []
      ends = []
      callbacks = app.callbacks(renderer, action)
      callbacks.each do |callback|
        env = env.merge(AUTHENTICATED => Ivar.new(writes: [], definite: true)) if callback.method == 'authenticate_user!' && callback.always
        facts = expand(renderer, callback.facts, Set.new)
        facts = conditional(facts) unless callback.always
        env = writes_env(env, facts)
        visits.concat(render_visits(renderer, facts.renders, env, default: action))
        # A callback that always renders or redirects stops the chain.
        return record_env(renderer, env, visits) if responds?(facts)

        callback_end = callback_end(renderer, callback, facts)
        ends << callback_end if callback_end
      end
      @callback_envs[renderer.name] << env
      facts = collect(renderer, action.to_sym, Set.new)
      env = writes_env(env, facts)
      own = render_visits(renderer, facts.renders, env, default: action)
      unless responds?(facts) || always_ends?(renderer, action.to_sym)
        logical = app.resolve(action, renderer.prefixes, partial: false)
        own << Visit.new(logical:, renderer:, env:, layouts: renderer.layouts, origin: action_end(renderer, action)) if logical
      end
      _body, definition, file = app.method_body(renderer.klass, action.to_sym)
      record_ending_calls(renderer, definition, file) if definition.is_a?(Prism::DefNode)
      own_writes = facts.writes.to_set(&:name)
      own.each do |visit|
        next if visit.origin.nil? || ends.empty?

        visit.origin = visit.origin.dup.tap do |origin|
          origin.callbacks = ends
          origin.own_writes = own_writes
        end
      end
      record_env(renderer, env, visits + own)
    end

    # The end of a callback's method and the ivars it uses, or nil for a block or a method without Ruby source.
    def callback_end(renderer, callback, facts)
      _body, definition, file = app.method_body(renderer.klass, callback.method.to_sym)
      return nil unless definition.is_a?(Prism::DefNode)

      record_ending_calls(renderer, definition, file)
      [method_end(renderer, definition, file), Code.ivar_names(definition) | facts.writes.to_set(&:name)]
    end

    def record_ending_calls(renderer, definition, file)
      Code.statement_calls(definition).each do |call|
        next if call.block

        ends = call.name == :render || Code::RESPONDS.include?(call.name) ||
               (app.follow?(renderer.klass, call.name) && method_outcomes(renderer, call.name) == Set[:ends])
        @ending_calls[[app.relative(file), call.location.start_offset]] = call if ends
      end
    end

    # The end of an action's method, where its default template renders; nil for an action without one (a route to
    # a template).
    def action_end(renderer, action)
      _body, definition, file = app.method_body(renderer.klass, action.to_sym)
      definition && method_end(renderer, definition, file)
    end

    def method_end(renderer, definition, file)
      exits = Code.returns(definition).reject do |node|
        node.arguments && Code.sequence(node.arguments.arguments) { |name| self_call_outcomes(renderer, name) }.include?(:ends)
      end
      Handoff.new(app.relative(file), definition, :after, nil, nil, nil, nil, exits)
    end

    def self_call_outcomes(renderer, name) = app.follow?(renderer.klass, name) ? method_outcomes(renderer, name) : Code::CONTINUES

    def record_env(renderer, env, visits)
      env.each do |name, ivar|
        next if name.start_with?('!')

        writes = @renderer_ivars[renderer.name][name]
        ivar.writes.each { |write| writes << write unless writes.any? { |w| w.key == write.key } }
      end
      visits
    end

    def responds?(facts) = facts.renders.any?(&:definite) || facts.responds.any?(&:definite)

    # Whether every path through a method renders, responds or raises (`product_update`'s `if` renders and returns,
    # and the rest redirects).
    def always_ends?(renderer, name) = method_outcomes(renderer, name) == Set[:ends]

    # A method's outcomes for its caller: a `return` in it continues the caller. A recursive call continues.
    def method_outcomes(renderer, name)
      key = [renderer.name, name]
      return @outcomes[key] if @outcomes.key?(key)

      @outcomes[key] = Code::CONTINUES
      body, = app.method_body(renderer.klass, name)
      return Code::CONTINUES if body.nil?

      found = Code.outcomes(body) { |called| self_call_outcomes(renderer, called) }
      @outcomes[key] = found.include?(:returns) ? (found - Set[:returns]) | Code::CONTINUES : found
    end

    # A method's facts, with the writes and renders of the renderer's own methods it calls.
    def collect(renderer, name, seen)
      collect_method(renderer, renderer.klass.instance_method(name), seen)
    rescue NameError
      Code::Facts.empty
    end

    # `call` (a Code::SelfCall) binds the method's parameters for its renders.
    # `seen` holds the methods being followed, so a recursive call stops; a method called twice is followed each
    # time, with each call's arguments (`render_page(:show)` after `render_invalid_link`, which calls it too).
    def collect_method(renderer, method, seen, call = nil)
      key = [method.owner, method.name]
      return Code::Facts.empty unless seen.add?(key)

      begin
        facts = @method_cache[key] ||= app.method_facts(method) || Code::Facts.empty
        facts = bind(facts, method, call) if call && facts.renders.any?
        expand(renderer, facts, seen, method)
      ensure
        seen.delete(key)
      end
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
        chain_writes = called.writes.to_set(&:name)
        called = conditional(called) unless call.definite
        result.writes.concat(called.writes)
        result.renders.concat(called.renders.map { |render| via(render, call, chain_writes) })
        result.responds.concat(called.responds)
      end
      result
    end

    # A render in a method `call` follows, as its caller sees it.
    def via(render, call, chain_writes)
      render.dup.tap do |copy|
        copy.via = [call] + (render.via || [])
        copy.chain_writes = chain_writes
      end
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
            Visit.new(logical:, renderer:, env:, layouts: nil, origin: render_handoff(render))
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
            Visit.new(logical: template, renderer:, env:, layouts:, origin: render_handoff(render))
          end
        end
      end
    end

    def render_handoff(render)
      call = render.via&.first
      return Handoff.new(render.file, render.node, :wrap) if call.nil?

      Handoff.new(render.file, render.node, :wrap, Handoff.new(call.file, call.node, :wrap), render.chain_writes)
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
                    .map { |logical| Visit.new(logical:, renderer:, env:, layouts: nil, origin: Handoff.new(render.file, render.node, :wrap)) }
      end
      node = first || options[:partial] || options[:layout]
      return [] if node.nil?

      logicals = partials(renderer, renderer.klass._helpers, node, render.scope)
      # `render @record` or `render SomeComponent.new(...)` isn't a partial we can name; a string we can't follow is.
      return string?(node) || options.key?(:partial) ? record_dynamic(render) : [] if logicals.empty?

      logicals.map do |logical|
        record_call(render, logical, call_locals(render, first, options, logical))
        Visit.new(logical:, renderer:, env:, layouts: nil, origin: Handoff.new(render.file, render.node, :wrap))
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
        Visit.new(logical:, renderer:, env:, layouts: nil, origin: Handoff.new(call.file, call.node, :wrap))
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
        sure = splats.flat_map { |splat| Code.hash_keys(splat.value, render.scope, index: false) }
        return splatted.to_h { |key| [key, sure.include?(key) ? nil : MAYBE] }.merge(options.to_h { |key, value| [key.to_s, value] })
      end

      locals = hash_locals(options[:locals])
      if options.key?(:collection) || options.key?(:object)
        as = Code.literal(options[:as]) || File.basename(logical).delete_prefix('_')
        locals[as] = options.key?(:collection) ? Element.new(options[:collection]) : options[:object]
        if options.key?(:collection)
          locals["#{as}_counter"] = Known.new('Integer')
          locals["#{as}_iteration"] = Known.new('ActionView::PartialIteration')
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

    # The partials that Ruby outside the controllers' actions and the views renders by name, with the locals it
    # passes: helpers (`render 'layouts/messages', column:`), components (`helpers.render('kaminari/nocount_paginator',
    # ...)`) and a controller's renderer anywhere (`ApplicationController.render(partial: 'templates/example', ...)`).
    # Only names with a slash: one without resolves with the prefixes of whichever controller renders the view.
    def record_ruby_renders
      Dir.glob(File.join(app.root, '{app,lib}/**/*.rb')).sort.each do |path|
        file = app.relative(path)
        view_code = file.start_with?('app/helpers/', 'app/components/')
        stack = [[Prism.parse_file(path).value, nil]]
        until stack.empty?
          node, scope = stack.pop
          scope = node if node.is_a?(Prism::DefNode)
          stack.concat(node.compact_child_nodes.map { |child| [child, scope] })
          next unless node.is_a?(Prism::CallNode) && node.name == :render
          next unless renderer_receiver?(node.receiver) || (view_code && view_receiver?(node.receiver))

          first, options = Code.render_arguments(node)
          name = Code.literal(first || options[:partial])
          next if name.nil? || !name.include?('/') || (first.nil? && (options.keys & NOT_TEMPLATES).any?)

          logical = app.resolve(name, [], partial: true) or next
          render = Code::Render.new(file:, node:, scope:, definite: false)
          record_call(render, logical, call_locals(render, first, options, logical))
        end
      end
    end

    # A controller class's renderer, which renders outside a request: `ApplicationController.render`,
    # `ApplicationController.renderer.render`, `ApplicationController.renderer.new(...).render`.
    def renderer_receiver?(node)
      node = node.receiver if node.is_a?(Prism::CallNode) && node.name == :new
      node = node.receiver if node.is_a?(Prism::CallNode) && node.name == :renderer
      (node.is_a?(Prism::ConstantReadNode) || node.is_a?(Prism::ConstantPathNode)) && node.slice.end_with?('Controller')
    end

    # self, or a component's `helpers` or `view_context`.
    def view_receiver?(node)
      node.nil? || node.is_a?(Prism::SelfNode) ||
        (node.is_a?(Prism::CallNode) && node.receiver.nil? && %i[helpers view_context].include?(node.name))
    end

    # A render the generator can't follow. Returns no visits.
    def record_dynamic(render)
      @dynamic_renders << [render.file, render.node.location.start_line, render.node.slice[0, 120]]
      []
    end
  end
end
