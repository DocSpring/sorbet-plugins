# frozen_string_literal: true

require_relative 'code'
require_relative 'names'

module ErbViews
  # What the running app knows about its views: the templates on disk, and each controller's and mailer's actions,
  # the callbacks that run for each, its template prefixes and its layout.
  class App
    # A controller or mailer. `layouts` are the layouts its actions may use (logical names, empty for none), and
    # `dynamic_layout` says one comes from code the generator can't follow.
    Renderer = Struct.new(:klass, :name, :kind, :prefixes, :actions, :layouts, :dynamic_layout, keyword_init: true)
    # A method or block that runs before an action: `facts` from its code, and whether it always runs for the action.
    Callback = Struct.new(:owner, :method, :facts, :always, keyword_init: true)
    # A helper that renders the partial its caller names (`render_code_example 'x/example', locals: {...}`): the
    # argument that holds the partial's name, and where its locals come from ([argument, hash key or nil], or nil).
    RenderWrapper = Struct.new(:partial, :locals, keyword_init: true)

    # Rails' own code, which the generator doesn't follow: its methods set Rails' private ivars, and `render` and
    # friends are facts already.
    RAILS_GEMS = %r{/(actionpack|actionview|actionmailer|activesupport|railties|activerecord|activemodel|sorbet-runtime)-[^/]+/}

    attr_reader :root, :templates, :components, :renderers

    def initialize(root)
      @root = root.to_s.end_with?('/') ? root.to_s : "#{root}/"
      @templates = index_templates
      @components = index_components
      @renderers = (controllers + mailers).sort_by(&:name)
    end

    # The template files for a logical name (`templates/show`, `templates/_row`): one per format.
    def files(logical) = @templates.fetch(logical, [])

    # The first logical name among `candidates` that has a template.
    def find(candidates) = candidates.find { |logical| @templates.key?(logical) }

    # Where `name` renders from a renderer with `prefixes`: a template (`render :edit`, `render 'templates/edit'`) or
    # a partial (`render 'row'`, `render partial: 'templates/row'`). nil if there's no such template in app/views.
    def resolve(name, prefixes, partial:)
      name = name.to_s.delete_prefix('/')
      underscore = partial ? '_' : ''
      if name.include?('/')
        dir, base = File.split(name)
        find(["#{dir}/#{underscore}#{base}"])
      else
        find(prefixes.map { |prefix| "#{prefix}/#{underscore}#{name}" })
      end
    end

    # Whether a partial name that isn't an ERB template in the app resolves, with any of `prefix_sets`, to another
    # template: one with another handler (`_ie9_warning.html`) or in a gem's or engine's views (Devise's, Kaminari's).
    def other_partial?(name, prefix_sets)
      name = name.delete_prefix('/')
      candidates = if name.include?('/')
                     dir, base = File.split(name)
                     ["#{dir}/_#{base}"]
                   else
                     prefix_sets.flatten.uniq.map { |prefix| "#{prefix}/_#{name}" }
                   end
      view_paths.any? { |path| candidates.any? { |candidate| Dir.glob(File.join(path, "#{candidate}.*")).any? } }
    end

    # The view directories that controllers and mailers render from: the app's, and its gems' and engines'.
    def view_paths
      @view_paths ||= [ActionController::Base, (ActionMailer::Base if defined?(ActionMailer::Base))].compact
                        .flat_map { |klass| klass.view_paths.to_a }
                        .filter_map { |resolver| resolver.respond_to?(:path) ? File.expand_path(resolver.path.to_s) : nil }
                        .uniq
    end

    # A layout's logical name, as ActionView::Layouts normalizes it: `layouts/` goes in front unless it's there.
    def self.layout_name(name) = name.match?(/\blayouts/) ? name : "layouts/#{name}"

    # Where a method is defined. sorbet-runtime replaces a method that has a sig with a wrapper defined in
    # sorbet-runtime, so this asks the signature for the original.
    def self.source_location(klass, name) = method_location(klass.instance_method(name))

    def self.method_location(method)
      signature = defined?(T::Utils) ? T::Utils.signature_for_method(method) : nil
      (signature&.method || method).source_location
    end

    # Every logical name a render name can reach. A name with unknown parts (Code::UNKNOWN, from Code.string_values) matches
    # the templates that fit it, under the first prefix that has any.
    def resolve_all(name, prefixes, partial:)
      return [resolve(name, prefixes, partial:)].compact unless name.include?("\0")

      name = name.delete_prefix('/')
      underscore = partial ? '_' : ''
      candidates = if name.include?('/')
                     dir, _, base = name.rpartition('/') # File.split refuses the NULs
                     ["#{dir}/#{underscore}#{base}"]
                   else
                     prefixes.map { |prefix| "#{prefix}/#{underscore}#{name}" }
                   end
      candidates.each do |candidate|
        pattern = Regexp.new("\\A#{candidate.split("\0", -1).map { |part| Regexp.escape(part) }.join('[^/]*')}\\z")
        found = @templates.keys.grep(pattern)
        return found.sort unless found.empty?
      end
      []
    end

    # The blocks in an ActiveAdmin registration that are controller code, not views: their renders are the actions'.
    ACTIVE_ADMIN_CONTROLLER_BLOCKS = %i[controller member_action collection_action page_action batch_action].freeze

    # ActiveAdmin's registrations (app/admin): the controller ActiveAdmin generates for each, and the code in the
    # registration's block (but not its actions), whose renders run in that controller's views.
    def active_admin_blocks
      return [] unless defined?(ActiveAdmin) && ActiveAdmin.application.namespaces.names.any?

      Dir.glob(File.join(@root, 'app/admin/**/*.rb')).sort.flat_map do |file|
        stack = [Prism.parse_file(file).value]
        found = []
        until stack.empty?
          node = stack.pop
          stack.concat(node.compact_child_nodes)
          next unless node.is_a?(Prism::CallNode) && node.receiver&.slice == 'ActiveAdmin' && node.block.is_a?(Prism::BlockNode)
          next unless %i[register register_page].include?(node.name)

          controller = active_admin_controller(node)
          renderer = controller && @renderers.find { |r| r.klass == controller }
          next if renderer.nil?

          views = (node.block.body&.body || []).reject do |statement|
            statement.is_a?(Prism::CallNode) && ACTIVE_ADMIN_CONTROLLER_BLOCKS.include?(statement.name)
          end
          views.each do |statement|
            facts = Code.scan(statement, relative(file))
            # `form partial: 'form'` renders the controller's partial.
            if statement.is_a?(Prism::CallNode) && statement.name == :form && Code.render_arguments(statement)[1].key?(:partial)
              facts.renders << Code::Render.new(file: relative(file), node: statement, scope: statement, definite: true)
            end
            found << [renderer, facts]
          end
        end
        found
      end
    end

    # The code a method runs (an UnboundMethod), or nil when it isn't in a file we can read.
    def method_facts(method)
      body, _scope, file = unbound_body(method)
      body && Code.scan(body, relative(file))
    end

    # A method's body, the scope its locals live in and its file: a `def`, or the block of a `define_method` (or an
    # ActiveAdmin action). nil when it isn't in a file we can read.
    def method_body(owner, name)
      unbound_body(owner.instance_method(name))
    rescue NameError
      nil
    end

    def unbound_body(method)
      file, line = App.method_location(method)
      return nil if file.nil? || !File.file?(file)

      node = Code.defs_by_line(file)[line] || Code.blocks_by_line(file)[line]
      return nil if node.nil? || node.body.nil?

      [node.body, node, file]
    rescue NameError
      nil
    end

    # Whether the render graph follows a call into `name`: the app's methods and gems' (Doorkeeper's `pre_auth`,
    # Devise's mailer helpers, which assign the ivars their templates read), but not Rails' own.
    def follow?(klass, name)
      return false if name == :render || Code::RESPONDS.include?(name)

      follow_method?(klass.instance_method(name))
    rescue NameError
      false
    end

    def follow_method?(method)
      file = App.method_location(method)&.first
      !file.nil? && !file.match?(RAILS_GEMS) && !file.include?('/sorbet/rbi/')
    end

    # The helper `name` in `helpers` (a renderer's helper module) if it renders a partial its caller names.
    def render_wrapper(helpers, name)
      @render_wrappers ||= {}
      key = [helpers, name]
      return @render_wrappers[key] if @render_wrappers.key?(key)

      @render_wrappers[key] = find_render_wrapper(helpers, name)
    end

    # The callbacks that run before `action`, in order.
    def callbacks(renderer, action)
      renderer.klass._process_action_callbacks.filter_map do |callback|
        next unless %i[before around].include?(callback.kind)

        always = true
        conditions = callback.instance_variable_get(:@if).map { |c| [c, false] } +
                     callback.instance_variable_get(:@unless).map { |c| [c, true] }
        runs = conditions.all? do |condition, negated|
          next true unless condition.is_a?(AbstractController::Callbacks::ActionFilter)

          # `only:` puts an ActionFilter in @if and `except:` puts one in @unless; either matches its own actions.
          hit = condition.instance_variable_get(:@actions).include?(action.to_s)
          negated ? !hit : hit
        end
        next unless runs

        always = false if conditions.any? { |condition, _| !condition.is_a?(AbstractController::Callbacks::ActionFilter) }
        facts = callback_facts(renderer.klass, callback.filter)
        Callback.new(owner: renderer, method: callback.filter.to_s, facts:, always:) if facts
      end
    end

    def app_file?(file) = file.start_with?(@root) && !file.include?('/vendor/') && !file.include?('/node_modules/')

    def active_admin_controller(call)
      argument = call.arguments&.arguments&.first
      ActiveAdmin.application.namespaces.each do |namespace|
        if call.name == :register && argument
          resource = namespace.resource_for(Object.const_get(argument.slice))
          return resource.controller if resource
        elsif call.name == :register_page && (name = Code.literal(argument))
          page = namespace.resources.find { |r| r.is_a?(ActiveAdmin::Page) && r.name == name }
          return page.controller if page
        end
      end
      nil
    rescue NameError
      nil
    end

    def relative(file) = file.delete_prefix(@root)

    private

    def find_render_wrapper(helpers, name)
      return nil unless helpers.method_defined?(name) || helpers.private_method_defined?(name)

      body, definition, file = method_body(helpers, name)
      return nil if body.nil? || !app_file?(file) || !definition.is_a?(Prism::DefNode) || definition.parameters.nil?

      parameters = (definition.parameters.requireds + definition.parameters.optionals).map(&:name)
      stack = [body]
      until stack.empty?
        node = stack.pop
        stack.concat(node.compact_child_nodes)
        next unless node.is_a?(Prism::CallNode) && node.name == :render && node.receiver.nil?

        _first, options = Code.render_arguments(node)
        partial = options[:partial]
        next unless partial.is_a?(Prism::LocalVariableReadNode) && parameters.include?(partial.name)

        return RenderWrapper.new(partial: parameters.index(partial.name), locals: wrapper_locals(options[:locals], parameters))
      end
      nil
    end

    # `locals: options[:locals]` => [index of options, :locals]; `locals: locals` => [index of locals, nil].
    def wrapper_locals(node, parameters)
      case node
      when Prism::LocalVariableReadNode
        [parameters.index(node.name), nil] if parameters.include?(node.name)
      when Prism::CallNode
        receiver = node.receiver
        key = node.name == :[] && Code.literal(node.arguments&.arguments&.first)
        [parameters.index(receiver.name), key.to_sym] if key && receiver.is_a?(Prism::LocalVariableReadNode) && parameters.include?(receiver.name)
      end
    end

    def index_templates
      found = Hash.new { |hash, key| hash[key] = [] }
      Dir.glob(File.join(@root, 'app/views/**/*.erb')).sort.each do |file|
        relative = relative(file).delete_prefix(Names::VIEWS_DIR)
        dir, base = File.split(relative)
        found[dir == '.' ? base.split('.').first : "#{dir}/#{base.split('.').first}"] << relative(file)
      end
      found.to_h
    end

    # Component template path => the component's class name.
    def index_components
      return {} unless defined?(ViewComponent::Base)

      ViewComponent::Base.descendants.each_with_object({}) do |component, found|
        next if component.name.nil?

        component.sidecar_files(['erb']).each do |file|
          found[relative(file)] = component.name if app_file?(file)
        end
      end
    end

    # The controllers a route reaches. A gem's controller that the app replaces (Devise::SessionsController, with
    # `devise_for :users, controllers: { sessions: 'users/sessions' }`) renders nothing.
    def controllers
      routed = routes.map(&:first).to_set
      ActionController::Base.descendants.filter_map do |klass|
        next if klass.name.nil? || klass.abstract? || !routed.include?(klass.name)

        renderer(klass, :controller, implicit_actions(klass))
      end
    end

    # Devise sends its emails through `Devise.mailer`; Devise::Mailer itself is only that class's parent unless it's
    # the one configured.
    def mailers
      return [] unless defined?(ActionMailer::Base)

      ActionMailer::Base.descendants.filter_map do |klass|
        next if klass.name.nil? || klass.abstract?
        next if defined?(Devise::Mailer) && klass == Devise::Mailer && Devise.mailer != Devise::Mailer

        renderer(klass, :mailer, [])
      end
    end

    def renderer(klass, kind, implicit)
      layouts, dynamic = layouts(klass)
      Renderer.new(klass:, name: klass.name, kind:, prefixes: klass._prefixes, actions: (klass.action_methods.to_a + implicit).uniq.sort,
                   layouts:, dynamic_layout: dynamic)
    end

    # Routes to an action with a template but no method (Rails renders the template).
    def implicit_actions(klass)
      routes.filter_map do |controller, action|
        next unless controller == klass.name && !klass.action_methods.include?(action)

        action if find(klass._prefixes.map { |prefix| "#{prefix}/#{action}" })
      end
    end

    # [controller class name, action] for every route. Rails dispatches `users/otp_credentials` to
    # Users::OtpCredentialsController by name, whatever that class's controller_path says (devise-otp's subclasses
    # keep `devise/otp_credentials`).
    def routes
      @routes ||= Rails.application.routes.routes.filter_map do |route|
        controller = route.defaults[:controller]
        action = route.defaults[:action]
        ["#{controller.camelize}Controller", action] if controller && action
      end.uniq
    end

    # The layouts a renderer's actions render in, as Rails picks them (ActionView::Layouts), and whether some come from
    # code we can't follow. A layout with `only:`/`except:` counts for every action: the contexts don't need to know
    # which.
    def layouts(klass)
      case (layout = klass._layout)
      when false then [[], false]
      when Symbol then method_layouts(klass, layout)
      when Proc then [[], true]
      when nil then [[implied_layout(klass)].compact, false]
      else
        logical = App.layout_name(layout.to_s)
        [@templates.key?(logical) ? [logical] : [], false]
      end
    end

    # Without a `layout` call, `layouts/<controller_path>` for the class or the nearest ancestor that has one.
    def implied_layout(klass)
      klass.ancestors.grep(Class).each do |ancestor|
        break if !ancestor.respond_to?(:abstract?) || ancestor.abstract?

        logical = "layouts/#{ancestor.controller_path}"
        return logical if @templates.key?(logical)
      end
      nil
    end

    # `layout :pick_layout`: the layouts named by string literals in that method.
    def method_layouts(klass, name)
      file, line = App.source_location(klass, name)
      node = file && Code.defs_by_line(file)[line]
      return [[], true] if node.nil?

      names = []
      stack = [node]
      until stack.empty?
        current = stack.pop
        names << current.unescaped if current.is_a?(Prism::StringNode)
        stack.concat(current.compact_child_nodes)
      end
      found = names.map { |n| App.layout_name(n) }.select { |logical| @templates.key?(logical) }
      [found.uniq, found.empty?]
    rescue NameError
      [[], true]
    end

    def callback_facts(klass, filter)
      case filter
      when Symbol, String
        method = begin
          klass.instance_method(filter.to_sym)
        rescue NameError
          nil
        end
        (method && method_facts(method)) || Code::Facts.empty
      when Proc
        file, line = filter.source_location
        return Code::Facts.empty unless file && File.file?(file)

        block = block_at(file, line)
        block ? Code.scan(block.body, relative(file)) : Code::Facts.empty
      else
        Code::Facts.empty
      end
    end

    def block_at(file, line)
      stack = [Prism.parse_file(file).value]
      until stack.empty?
        node = stack.pop
        return node if (node.is_a?(Prism::BlockNode) || node.is_a?(Prism::LambdaNode)) && node.location.start_line == line

        stack.concat(node.compact_child_nodes)
      end
      nil
    end
  end
end
