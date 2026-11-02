# frozen_string_literal: true

require 'fileutils'
require 'open3'
require 'set'
require 'tmpdir'
require_relative 'sorbet_type'

module ErbViews
  # The types of the instance variables and locals that reach each template, from Sorbet itself. The generator copies
  # the code (controllers, mailers, templates) outside the app with `T.reveal_type` around each ivar's assignments,
  # each local's value at its render, and each ivar where it passes to the next template (around a `render`, at the
  # end of an action, at the end of a template for its layout). It type-checks the copies in place of the originals
  # and reads the revealed types back. Each pass declares the last pass's types (the renderers' ivars in a probe RBI,
  # the templates' in their contexts), so a type flows from a callback to its action, from an action to its template
  # and from a template to its partials.
  class Types
    # A constant at the start of a type's name: not after `::`, `.` or a word character, and not a literal's text.
    CONSTANT = /(?<![\w:.'"])([A-Z]\w*(?:::[A-Z]\w*)*)/
    NIL = [:name, 'NilClass', nil].freeze
    REVEALED = %r{\A(.+?):(\d+): Revealed type: `(.*)` https://srb\.help/7014\n?\z}
    MAX_PASSES = 8

    # Text the probe adds to a copy: `open` before `start` and `close` after `finish` (a point when finish is nil),
    # or with `replace`, `open` in place of the bytes from start to finish. `reveals`: [offset of a `T.reveal_type`
    # in `open`, the key its type is found under].
    Insertion = Struct.new(:start, :finish, :open, :close, :reveals, :replace, keyword_init: true)

    # A type with every constant from the root (`T.nilable(User)` => `T.nilable(::User)`): the contexts live in
    # SorbetViews, where `Templates::Row` could resolve to a view class instead of the app's.
    def self.qualify(type) = type.gsub(CONSTANT) { |constant| constant == 'T' || constant.start_with?('T::') ? constant : "::#{constant}" }

    # `sorbet`: the Sorbet binary with the erb_views plugin. `rbi_dir`: where the app's generated RBIs are (relative to
    # the app), which the passes replace with their own.
    def initialize(app, graph, sorbet:, rbi_dir:, log: $stderr)
      @app = app
      @graph = graph
      @sorbet = sorbet
      @rbi_dir = rbi_dir
      @log = log
      @revealed = {}
    end

    # Runs the probe passes until the types stop changing. The block renders the contexts with the types so far.
    def infer!(&contexts)
      insertions = probe_insertions
      count = insertions.values.flatten.flat_map { |insertion| insertion.reveals.map(&:last) }.uniq.size
      probe_dir do |dir|
        root = File.join(dir, 'p')
        positions = write_probe(root, insertions)
        MAX_PASSES.times do |pass|
          File.write(File.join(root, 'erb_views.rbi'), contexts.call)
          File.write(File.join(root, 'renderer_ivars.rbi'), renderer_ivars_rbi)
          # A value in code that a pass's types make unreachable keeps the type an earlier pass found.
          found = @revealed.merge(run(dir, insertions.keys, positions))
          @log.puts "erb_views: types pass #{pass + 1}: #{found.size} of #{count} values revealed"
          changed = found != @revealed
          @revealed = found
          break unless changed
        end
        log_unrevealed(positions) if ENV['ERB_VIEWS_DEBUG']
      end
    end

    # A temporary directory for the copies, or with ERB_VIEWS_PROBE_DIR set, that directory (emptied first and kept
    # afterwards, to read the copies and run Sorbet on them by hand).
    def probe_dir(&)
      keep = ENV.fetch('ERB_VIEWS_PROBE_DIR', '')
      return Dir.mktmpdir('erb_views_types', &) if keep.empty?

      FileUtils.rm_rf(keep)
      FileUtils.mkdir_p(keep)
      yield keep
    end

    # An ivar in a template's context: what it is at each handoff into the template (nil where it isn't set), and
    # what the template assigns.
    def ivar_type(logical, file, name)
      types = arrival_types(logical, name)
      own = @graph.template_facts(file).writes.select { |write| write.name == name }.filter_map { |write| write_type(write) }
      types << NIL if types.empty? && own.any?
      types.concat(own)
      return 'T.untyped' if types.empty?

      render(SorbetType.union(types))
    end

    # What an ivar is where the template's renderers hand over to it (a SorbetType), or nil when none does.
    def ivar_arrival(logical, name)
      types = arrival_types(logical, name)
      types.empty? ? nil : SorbetType.union(types)
    end

    def arrival_types(logical, name)
      context = @graph.contexts.key?(logical) ? @graph.contexts[logical] : nil
      (context&.origins || {}).values.map do |handoff, env|
        env.key?(name) ? handoff_type(handoff, name, env.fetch(name)) : NIL
      end
    end

    # The union of what every caller passes; a caller that leaves the local out makes it nilable. For a partial's sig
    # comment (`sig: true`): constants as the app writes them (`T.nilable(User)`), and the union of the values Sorbet
    # could type, as a caller that passes an untyped value passes any sig.
    def local_type(logical, name, sig: false)
      calls = @graph.contexts.key?(logical) ? @graph.contexts[logical].calls : []
      return 'T.untyped' if calls.empty?

      types = calls.map { |call| call.locals.key?(name) ? value_type(call.file, call.locals[name]) : NIL }
      types -= [SorbetType::UNTYPED] if sig && (types - [SorbetType::UNTYPED, NIL]).any?
      type = SorbetType.render(SorbetType.union(types))
      sig ? type : Types.qualify(type)
    end

    private

    def render(type) = Types.qualify(SorbetType.render(type))

    # The narrower of Sorbet's type at the handoff and the union of the ivar's assignments that reach it (nilable
    # when one may not run, unless one runs on every path to the render: the same block's). Sorbet knows the flow inside the method (`return unless @record`), but an ivar the method
    # doesn't assign has the type the whole class declares: nilable when one action's callbacks don't set it, and
    # nilable for one a method the action calls sets. The assignments know which ones run before this handoff.
    def handoff_type(handoff, name, ivar)
      place = handoff&.outer && !handoff.inner.include?(name) ? handoff.outer : handoff
      types = ivar.writes.filter_map { |write| write_type(write) }
      assigned = if types.empty? then nil
                 elsif ivar.definite || handoff&.dominating&.include?(name) then SorbetType.union(types)
                 else SorbetType.union(types + [NIL])
                 end
      callback = handoff && last_callback(handoff, name)
      candidates = [place && @revealed[[:handoff, place.key, name]], callback && @revealed[[:handoff, callback.key, name]], assigned]
      candidates = candidates.compact - [SorbetType::UNTYPED]
      return SorbetType::UNTYPED if candidates.empty?

      candidates.drop(1).reduce(candidates.first) do |best, type|
        SorbetType.subtype?(type, best) && !SorbetType.subtype?(best, type) ? type : best
      end
    end

    # The end of the last callback before an action's handoff that uses the ivar, unless the action assigns it.
    def last_callback(handoff, name)
      return nil if handoff.callbacks.nil? || handoff.own_writes.include?(name)

      handoff.callbacks.reverse_each { |callback, names| return callback if names.include?(name) }
      nil
    end

    # nil for an operator write (`@a += 1`), which keeps the type of the ivar's other writes.
    def write_type(write)
      return SorbetType::UNTYPED if write.node.is_a?(Prism::InstanceVariableTargetNode) # `@a, @b = pair`
      return nil if write.value.nil?

      revealed = @revealed.fetch(value_key(write.file, write.value), SorbetType::UNTYPED)
      revealed == SorbetType::UNTYPED ? let_type(write.value) || revealed : revealed
    end

    # The type a `T.let` declares, for a value Sorbet can't type (`@plans = T.let(load_plans, T::Array[Plan])`).
    def let_type(node)
      return nil unless node.is_a?(Prism::CallNode) && node.name == :let && node.receiver&.slice == 'T'

      type = node.arguments&.arguments&.[](1)
      type && SorbetType.parse(type.slice)
    end

    def value_type(file, value)
      case value
      when nil, RenderGraph::MAYBE then SorbetType::UNTYPED
      when RenderGraph::Known then SorbetType.parse(value.type)
      when RenderGraph::Element then SorbetType.element(@revealed.fetch(value_key(file, value.node), SorbetType::UNTYPED))
      else
        revealed = @revealed.fetch(value_key(file, value), SorbetType::UNTYPED)
        revealed == SorbetType::UNTYPED ? let_type(value) || revealed : revealed
      end
    end

    def value_key(file, node) = [file, node.location.start_offset, node.location.end_offset]

    # file => [Insertion] for the app's files.
    def probe_insertions
      found = Hash.new { |hash, file| hash[file] = [] }
      value_nodes.each do |file, node|
        next unless probed?(file)

        key = value_key(file, node)
        next if found[file].any? { |insertion| insertion.reveals.any? { |_, k| k == key } }

        found[file] << value_insertion(node, key)
        found[file].concat(unwrap_let(node))
      end
      handoffs.each do |key, (handoff, names)|
        found[handoff.file].concat(handoff_insertions(handoff, key, names.sort))
      end
      @graph.ending_calls.each do |(file, offset), _call|
        found[file] << Insertion.new(start: offset, finish: nil, open: 'return ', close: nil, reveals: []) if probed?(file)
      end
      found
    end

    def probed?(file) = !file.start_with?('/') && File.file?(File.join(@app.root, file))

    # `T.reveal_type((value))` in place of the value. A shorthand hash value (`render 'row', form:`) spans its
    # `form:`, so the copy spells it out: `form: T.reveal_type((form))`.
    def value_insertion(node, key)
      start = node.location.start_offset
      finish = node.location.end_offset
      return Insertion.new(start:, finish:, open: 'T.reveal_type((', close: '))', reveals: [[0, key]]) unless node.is_a?(Prism::ImplicitNode)

      name = node.slice.delete_suffix(':')
      prefix = "#{name}: "
      Insertion.new(start:, finish:, open: "#{prefix}T.reveal_type((#{name}))", reveals: [[prefix.bytesize, key]], replace: true)
    end

    # `@stats = T.let(Stats.for(key), T.nilable(T::Hash[String, Integer]))` declares the ivar nilable for the
    # actions that don't set it, and Sorbet then types it as declared. The copy assigns `(Stats.for(key))`, so the
    # type at the handoff is what this action assigns (or the `T.let`'s type when Sorbet can't type the value).
    def unwrap_let(node)
      return [] unless node.is_a?(Prism::CallNode) && node.name == :let && node.receiver&.slice == 'T'

      inner = node.arguments&.arguments&.first
      return [] if inner.nil?

      [Insertion.new(start: node.location.start_offset, finish: inner.location.start_offset, open: '(', reveals: [], replace: true),
       Insertion.new(start: inner.location.end_offset, finish: node.location.end_offset, open: ')', reveals: [], replace: true)]
    end

    # [file, node] for every value whose type the contexts use: the ivar writes that reach a template or a renderer,
    # the templates' own writes, and the locals' values at each render.
    def value_nodes
      nodes = []
      @graph.contexts.each_value do |context|
        context.ivars.each_value { |ivar| ivar.writes.each { |write| nodes << [write.file, write.value] } }
        context.calls.each do |call|
          call.locals.each_value do |value|
            node = value.is_a?(RenderGraph::Element) ? value.node : value
            nodes << [call.file, node] if node.is_a?(Prism::Node)
          end
        end
      end
      @graph.renderer_ivars.each_value { |ivars| ivars.each_value { |writes| writes.each { |write| nodes << [write.file, write.value] } } }
      template_files.each { |file| @graph.template_facts(file).writes.each { |write| nodes << [file, write.value] } }
      nodes.reject { |_, node| node.nil? }
    end

    # handoff key => [Handoff, the ivars that pass there].
    def handoffs
      found = {}
      add = lambda do |handoff, names|
        next if names.empty? || !probed?(handoff.file)

        entry = found[handoff.key] ||= [handoff, Set.new]
        entry[1].merge(names)
      end
      @graph.contexts.each_value do |context|
        context.origins.each_value do |handoff, env|
          next if handoff.nil?

          names = env.keys.reject { |name| name.start_with?('!') }
          names.each do |name|
            callback = last_callback(handoff, name)
            add.call(callback, [name]) if callback
          end
          if handoff.outer
            inner = names.select { |name| handoff.inner.include?(name) }
            add.call(handoff, inner)
            add.call(handoff.outer, names - inner)
          else
            add.call(handoff, names)
          end
        end
      end
      found
    end

    # The reveals at a handoff. An `:after` handoff also reveals at each of its exits (`(T.reveal_type(@a); return)
    # if done`), under the same keys: the run takes the union.
    def handoff_insertions(handoff, key, names)
      case handoff.kind
      when :wrap then [wrap_insertion(handoff.node, key, names)]
      when :after
        body = handoff.node.body
        body = body.statements if body.is_a?(Prism::BeginNode)
        return [] if body.nil?

        text, reveals = reveal_text(key, names, prefix: '; ')
        [Insertion.new(start: body.location.end_offset, finish: nil, open: text.chomp('; '), close: nil, reveals:),
         *(handoff.exits || []).map { |exit| wrap_insertion(exit, key, names) }]
      when :end
        text, reveals = reveal_text(key, names, prefix: '<% ')
        [Insertion.new(start: File.size(File.join(@app.root, handoff.file)), finish: nil, open: "#{text}%>", close: nil, reveals:)]
      else raise ArgumentError, "unknown handoff kind #{handoff.kind.inspect}"
      end
    end

    def wrap_insertion(node, key, names)
      text, reveals = reveal_text(key, names, prefix: '(')
      Insertion.new(start: node.location.start_offset, finish: node.location.end_offset, open: text, close: ')', reveals:)
    end

    def reveal_text(key, names, prefix:)
      text = +prefix
      reveals = names.map do |name|
        offset = text.bytesize
        text << "T.reveal_type(#{name}); "
        [offset, [:handoff, key, name]]
      end
      [text, reveals]
    end

    def template_files = Dir.glob(File.join(@app.root, '{app/views,app/components}/**/*.erb')).map { |file| @app.relative(file) }

    # Writes each file's copy, and returns where each reveal is in the copies: [file, line] => [[column, key]], with
    # byte columns.
    def write_probe(root, insertions)
      positions = Hash.new { |hash, place| hash[place] = [] }
      insertions.each do |file, list|
        source, shift = typed_true(file, File.binread(File.join(@app.root, file)))
        # At one offset: ranges that end there close (inner first), then points, then ranges open (outer first),
        # then replacements.
        events = list.flat_map do |insertion|
          if insertion.replace
            [[insertion.start + shift, 3, 0, insertion]]
          elsif insertion.finish.nil?
            [[insertion.start + shift, 1, 0, insertion]]
          else
            size = insertion.finish - insertion.start
            [[insertion.start + shift, 2, -size, insertion], [insertion.finish + shift, 0, size, insertion]]
          end
        end
        out = +''.b
        at = 0
        events.sort_by { |offset, kind, size, _| [offset, kind, size] }.each do |offset, kind, _size, insertion|
          out << source.byteslice(at...offset)
          at = offset
          if kind.zero?
            out << insertion.close.b
            next
          end
          line = out.count("\n") + 1
          column = out.bytesize - ((out.rindex("\n") || -1) + 1)
          insertion.reveals.each { |offset_in, key| positions[[file, line]] << [column + offset_in, key] }
          out << insertion.open.b
          at = insertion.finish + shift if kind == 3
        end
        out << source.byteslice(at..)
        path = File.join(root, file)
        FileUtils.mkdir_p(File.dirname(path))
        File.binwrite(path, out)
      end
      positions
    end

    # A Ruby file's copy is `typed: true`, so Sorbet infers its methods' bodies. Returns the source and how far its
    # code moved.
    def typed_true(file, source)
      return [source, 0] if file.end_with?('.erb')

      sigil = source.match(/^# typed: (\w+)/)
      return ["# typed: true\n".b + source, 14] if sigil.nil?
      return [source, 0] if %w[true strict strong].include?(sigil[1])

      copy = source.byteslice(0...sigil.begin(1)) + 'true'.b + source.byteslice(sigil.end(1)..)
      [copy, copy.bytesize - source.bytesize]
    end

    def run(dir, files, positions)
      ignores = (files + [@rbi_dir]).map { |file| "--ignore=/#{file}" }
      command = [@sorbet, '--silence-dev-message', '--allowed-extension=.rb,.rbi,.erb', '--enable-plugin=erb_views', *ignores, dir]
      output, _status = Open3.capture2e(*command, chdir: @app.root.to_s)
      lines = output.lines
      found = {}
      unmatched = Hash.new { |hash, place| hash[place] = Unmatched.new(positions.fetch(place, [])) }
      lines.each_with_index do |line, index|
        match = REVEALED.match(line) or next
        file = match[1].split('/p/', 2)[1] or next
        key = unmatched[[file, match[2].to_i]].take(caret_column(lines[index + 1], lines[index + 2]))
        next if key.nil?

        parsed = SorbetType.parse(match[3])
        type = parsed ? SorbetType.widen(parsed) : SorbetType::UNTYPED
        found[key] = found.key?(key) ? SorbetType.union([found[key], type]) : type
      end
      raise "Sorbet revealed no types; its output began:\n#{output[0, 2000]}" if found.empty? && positions.any?

      found
    end

    # The reveals at one line of a copy that no report has matched yet: each report takes the first at its column, or
    # else the line's first. Each lookup skips the ones already taken, so a line with many reveals (every ivar around a
    # render) takes linear time.
    class Unmatched
      # `entries`: [column, key] in the line's order.
      def initialize(entries)
        @order = entries.map(&:last)
        @by_column = entries.group_by(&:first).transform_values { |list| list.map(&:last) }
        @taken = Set.new
        @next = 0
      end

      def take(column)
        list = @by_column[column]
        list.shift while list&.any? && @taken.include?(list.first)
        key = list&.shift
        if key.nil?
          @next += 1 while @next < @order.size && @taken.include?(@order[@next])
          key = @order[@next]
        end
        @taken << key unless key.nil?
        key
      end
    end

    # With ERB_VIEWS_DEBUG set: the reveals Sorbet never reached (code it found unreachable).
    def log_unrevealed(positions)
      positions.each do |(file, line), reveals|
        reveals.each { |_, key| @log.puts "erb_views: unrevealed #{file}:#{line} #{key.inspect[0, 160]}" unless @revealed.key?(key) }
      end
    end

    # The caret's column under a snippet line (`    12 |    @a = T.reveal_type((1))`), or nil.
    def caret_column(snippet, carets)
      return nil unless snippet&.include?('|') && carets&.include?('^')

      carets.index('^') - (snippet.index('|') + 1)
    end

    # Each renderer's ivars with the types its assignments have, for Sorbet to use in the methods that read them:
    # nilable unless the callbacks always set it before every action, so the flow typing in each action decides.
    def renderer_ivars_rbi
      out = +"# typed: true\n"
      @graph.renderer_ivars.sort.each do |name, ivars|
        envs = @graph.callback_envs[name]
        declarations = ivars.sort.filter_map do |ivar, writes|
          types = writes.filter_map { |write| write_type(write) }
          next if types.empty?

          type = SorbetType.strip_nil(SorbetType.union(types))
          next if type == SorbetType::UNTYPED || type == NIL

          set = envs.any? && envs.all? { |env| env[ivar]&.definite }
          "    #{ivar} = T.let(T.unsafe(nil), #{render(set ? type : SorbetType.union([type, NIL]))})"
        end
        next if declarations.empty?

        out << "\nclass ::#{name}\n  sig { void }\n  def initialize\n#{declarations.join("\n")}\n  end\nend\n"
      end
      out
    end
  end
end
