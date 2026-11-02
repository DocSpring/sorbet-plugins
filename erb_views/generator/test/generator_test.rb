# frozen_string_literal: true

# Tests for the generator's parts that don't need a Rails app: reading Sorbet's types, the code facts the render graph
# and the type probe use, partials' locals, and the parts that must match the plugin (the templates' Ruby and class
# names), checked against the plugin itself when SORBET names the binary. The rest of the generator runs against the
# app it generates for.
#
#   SORBET=path/to/sorbet ruby erb_views/generator/test/generator_test.rb

ENV['MT_NO_PLUGINS'] = '1' # other gems' Minitest plugins can load conflicting dependencies
require 'minitest/autorun'
require 'set'
require 'open3'
require_relative '../code'
require_relative '../ivar_hints'
require_relative '../names'
require_relative '../partial_locals'
require_relative '../sorbet_type'
require_relative '../template_ruby'

class SorbetTypeTest < Minitest::Test
  def type(text) = ErbViews::SorbetType.widen(ErbViews::SorbetType.parse(text))
  def render(type) = ErbViews::SorbetType.render(type)
  def union(*texts) = render(ErbViews::SorbetType.union(texts.map { |text| type(text) }))

  def test_widens_literals_tuples_and_shapes
    assert_equal 'Integer', render(type('Integer(1)'))
    assert_equal 'String', render(type('String("a")'))
    assert_equal 'T::Boolean', render(type('TrueClass'))
    assert_equal 'T::Array[T.any(Integer, String)]', render(type('[Integer(1), String] (2-tuple)'))
    assert_equal 'T::Hash[Symbol, Integer]', render(type('{a: Integer(1), b: Integer(2)} (shape of T::Hash[T.untyped, T.untyped])'))
    assert_equal 'T.nilable(T::Array[String])', render(type('T.nilable(T::Array[String])'))
  end

  def test_keeps_tuples_inside_type_arguments
    assert_equal 'T::Array[[Contact, String]]', render(type('T::Array[[Contact, String("a")]]'))
  end

  def test_unions
    assert_equal 'T.nilable(Integer)', union('Integer', 'NilClass')
    assert_equal 'T::Boolean', union('TrueClass', 'FalseClass')
    assert_equal 'T.untyped', union('Integer', 'T.untyped')
    assert_equal 'T.nilable(T.any(Integer, String))', union('T.nilable(Integer)', 'String')
    assert_equal 'NilClass', union('NilClass')
  end

  def test_unknown_text_parses_as_nil
    assert_nil ErbViews::SorbetType.parse('Integer(')
  end

  def test_subtype
    subtype = ->(sub, sup) { ErbViews::SorbetType.subtype?(type(sub), type(sup)) }
    assert subtype.call('Integer', 'T.nilable(Integer)')
    refute subtype.call('T.nilable(Integer)', 'Integer')
    assert subtype.call('Integer', 'T.any(Integer, String)')
    assert subtype.call('Integer', 'Numeric')
    assert subtype.call('String', 'T.untyped')
    refute subtype.call('T.untyped', 'String')
  end

  def test_collection_elements
    element = ->(text) { render(ErbViews::SorbetType.element(type(text))) }
    assert_equal 'String', element.call('T::Array[String]')
    assert_equal 'User', element.call('User::PrivateAssociationRelation')
    assert_equal 'T.untyped', element.call('Integer')
  end
end

class CodeTest < Minitest::Test
  def method_node(source) = Prism.parse(source).value.statements.body.first

  def outcomes(source, ends: [])
    ErbViews::Code.outcomes(method_node(source).body) { |name| ends.include?(name) ? Set[:ends] : ErbViews::Code::CONTINUES }
  end

  def test_a_method_that_renders_or_redirects_on_every_path_ends
    assert_equal Set[:ends], outcomes(<<~RUBY)
      def product_update
        @entry = find(params[:slug])
        if @entry
          render
          return
        end
        raise NotFound unless legacy_id
        redirect_to updates_path
      end
    RUBY
  end

  def test_an_early_return_falls_through_to_the_default_template
    assert_equal Set[:returns, :continue], outcomes(<<~RUBY)
      def pricing
        return if enterprise?
        @plans = plans
      end
    RUBY
    assert_equal Set[:ends, :continue], outcomes("def show\n  redirect_to(root_path) and return if x\nend")
  end

  def test_branches_need_an_else_to_end
    assert_equal Set[:ends, :continue], outcomes("def a\n  if x then render :a end\nend")
    assert_equal Set[:ends], outcomes("def a\n  if x then render :a else head :ok end\nend")
    assert_equal Set[:ends], outcomes("def a\n  case x\n  when 1 then render :a\n  else redirect_to b\n  end\nend")
    assert_equal Set[:ends, :continue], outcomes("def a\n  case x\n  when 1 then render :a\n  end\nend")
  end

  def test_a_return_that_responds_ends
    assert_equal Set[:ends], outcomes("def a\n  return head(:not_found) unless x\n  render :a\nend")
  end

  def test_a_return_after_performed_ends
    assert_equal Set[:ends, :continue], outcomes("def a\n  load_report\n  return if performed?\n  @b = 1\nend")
    node = method_node("def a\n  return if performed?\n  return unless x\n  @b = 1\nend")
    assert_equal [3], ErbViews::Code.returns(node).map { |exit| exit.location.start_line } # not `return if performed?`
  end

  def test_self_calls_use_the_callees_outcomes
    assert_equal Set[:ends], outcomes("def a\n  render_page(:a)\nend", ends: [:render_page])
    assert_equal Set[:continue], outcomes("def a\n  helper\nend")
  end

  def test_a_rescue_adds_its_paths
    assert_equal Set[:ends, :continue], outcomes("def a\n  begin\n    render :a\n  rescue StandardError\n    log\n  end\nend")
  end

  def test_statement_calls_skip_blocks_and_receivers
    node = method_node(<<~RUBY)
      def prepare
        @pack = find
        redirect_back_with(alert: 'x') unless @pack
        items.each { |item| head(:gone) }
        response.redirect_to(x)
        self.render(:a)
      end
    RUBY
    # `find` is a value, not a statement.
    assert_equal %i[redirect_back_with render], ErbViews::Code.statement_calls(node).map(&:name)
  end

  def test_returns_skip_blocks_and_lambdas
    node = method_node("def a\n  return if x\n  list.each { return }\n  f = -> { return }\n  return 1\nend")
    assert_equal 2, ErbViews::Code.returns(node).size
  end

  def test_hash_keys_follow_a_local_and_the_keys_it_is_given
    scope = Prism.parse("options = { status: 1 }\noptions[:label] = 2 if x\nrender 'a', **options").value
    splat = scope.statements.body.last.arguments.arguments.last.elements.first
    assert_equal %w[status label], ErbViews::Code.hash_keys(splat.value, scope)
  end

  TABS = %w[general privacy].freeze

  def test_a_block_parameter_over_a_constant_list
    scope = Prism.parse("CodeTest::TABS.each do |tab|\n  render \"settings/\#{tab}\"\nend\nOTHER.each { |tab| x }").value
    render = scope.statements.body.first.block.body.body.first
    # OTHER isn't defined, so `tab` could be anything.
    assert_equal ["settings/#{ErbViews::Code::UNKNOWN}"], ErbViews::Code.string_values(render.arguments.arguments.first, scope)
    scope = Prism.parse("CodeTest::TABS.each do |tab|\n  render \"settings/\#{tab}\"\nend").value
    render = scope.statements.body.first.block.body.body.first
    assert_equal %w[settings/general settings/privacy], ErbViews::Code.string_values(render.arguments.arguments.first, scope)
  end

  def test_ivar_names
    node = method_node("def a\n  @a = 1\n  @b ||= @c\n  @d += 1\n  def inner = @e\nend")
    assert_equal Set['@a', '@b', '@c', '@d'], ErbViews::Code.ivar_names(node)
  end

  def test_scan_marks_what_may_not_run
    facts = ErbViews::Code.scan(method_node(<<~RUBY).body, 'app/controllers/a.rb')
      def show
        @a = 1
        @b = 2 if x
        return unless y
        @c = 3
        render :show
      end
    RUBY
    assert_equal({ '@a' => true, '@b' => false, '@c' => false }, facts.writes.to_h { |write| [write.name, write.definite] })
    refute facts.renders.first.definite
  end

  def test_scan_records_the_writes_on_every_path_to_a_render
    facts = ErbViews::Code.scan(method_node(<<~RUBY).body, 'app/controllers/a.rb')
      def index
        @a = 1
        @b = 2 if x
        respond_to do |format|
          format.html do
            @c = 3
            render :index
          end
          format.csv { render :csv }
        end
        if y
          @d = 4
        else
          render :empty
        end
        case z
        when 1 then @e = 5
        else render :other
        end
        begin
          @f = 6
          render :body
        rescue StandardError
          render :rescued
        else
          render :finished
        ensure
          @g = 7
        end
        render :last
      end
    RUBY
    dominating = facts.renders.to_h { |render| [render.node.arguments.arguments.first.unescaped, render.dominating.to_a.sort] }
    assert_equal(
      {
        'index' => %w[@a @c], 'csv' => %w[@a], 'empty' => %w[@a], 'other' => %w[@a], 'body' => %w[@a @f],
        'rescued' => %w[@a], 'finished' => %w[@a @f], 'last' => %w[@a @g],
      },
      dominating
    )
  end
end

class PartialLocalsTest < Minitest::Test
  def locals(source) = ErbViews::PartialLocals.read(source)

  def test_reads_the_locals_and_their_types
    partial = locals(<<~ERB)
      <%# locals: (template:, title: 'Card', count: 1, **) -%>
      <%# sig { params(template: Template, count: T.nilable(Integer)) } -%>
      <p><%= template.name %></p>
    ERB
    assert_equal %w[template title count], partial.names
    assert_equal [true, false, false], partial.params.map(&:required)
    assert_equal ['Template', nil, 'T.nilable(Integer)'], partial.params.map(&:type)
    assert_equal <<~RBI.chomp, partial.method_rbi('__partial__templates__card')
      \  sig { params(template: Template, title: T.untyped, count: T.nilable(Integer), __rest: T.untyped).returns(T.untyped) }
      \  def __partial__templates__card(template:, title: T.unsafe(nil), count: T.unsafe(nil), **__rest); end
    RBI
  end

  def test_a_multi_line_comment_and_a_named_rest
    partial = locals("<%#  locals: (a:,\n  **opts) %>\n<%# sig { params(opts: String) } %>\n")
    assert_equal ['a'], partial.names
    assert_equal ['opts', 'String'], [partial.rest, partial.rest_type]
  end

  def test_no_locals
    assert_equal "  sig { returns(T.untyped) }\n  def __partial__x; end", locals("<%# locals: () %>\n").method_rbi('__partial__x')
    assert_nil locals("<p><%= x %></p>\n")
    assert_nil locals("<%#locals: (a:) %>\n") # Rails wants a space after the `#`
  end

  def test_only_the_first_locals_comment_counts
    assert_equal ['a'], locals("<%# locals: (a:) %>\n<%# locals: (b:) %>\n").names
  end
end

class IvarHintsTest < Minitest::Test
  def hints(source) = ErbViews::IvarHints.read(source, 'x.html.erb')

  def test_reads_each_declaration
    assert_equal({ '@template' => 'Template', '@pairs' => 'T::Hash[String, [Integer, String]]', '@token' => 'T.nilable(ApiToken)' },
                 hints("<%# ivars: (@template: Template, @pairs: T::Hash[String, [Integer, String]],\n  @token: T.nilable(ApiToken)) -%>\n"))
    assert_equal({}, hints("<%# ivars are set by the controller %>\n"))
  end

  def test_refuses_what_it_cannot_read
    error = assert_raises(ArgumentError) { hints('<%# ivars: (template: Template) %>') }
    assert_equal 'x.html.erb: can\'t read "template: Template" in its ivars comment', error.message
    assert_raises(ArgumentError) { hints("<%# ivars: (@a: Integer) %>\n<%# ivars: (@a: String) %>") }
  end
end

class PartialRendersTest < Minitest::Test
  def names(erb) = ErbViews::Code.partial_render_names(Prism.parse(ErbViews::TemplateRuby.call(erb)).value)

  def test_literal_renders
    assert_equal %w[a b c d e], names(<<~ERB)
      <%= render 'a' %><%= render 'b', x: 1 %><%= render('c', { x: 1 }) %>
      <%= render partial: 'd' %><%= render partial: 'e', locals: { x: 1 } %>
    ERB
  end

  def test_renders_that_stay_renders
    assert_empty names(<<~ERB)
      <%= render name %><%= render 'a' do %>x<% end %><%= render 'b', **opts %><%= render 'c', 'x' => 1 %>
      <%= render partial: 'd', collection: [] %><%= render partial: 'e', locals: opts %><%= helper.render 'f' %>
      <%= render :g %><%= render "h\#{i}" %><%= render('j', { 'x' => 1 }) %><%= render 'k', a: 1, 'b' => 2 %>
    ERB
  end

  def test_method_names
    assert_equal '__partial__templates__row', ErbViews::Names.partial_method('templates/row')
    [%w[/a a/ a//b a.html a-b] + ['']].flatten.each { |name| assert_nil ErbViews::Names.partial_method(name), name }
  end
end

# The generator's copies of the plugin's transform and class names, against the plugin, for every fixture template.
class PluginConformanceTest < Minitest::Test
  FIXTURES = File.expand_path('../../test', __dir__)

  def setup
    @sorbet = ENV.fetch('SORBET', '')
    skip 'set SORBET to the Sorbet binary with the erb_views plugin' if @sorbet.empty?
    @templates = Dir.glob('app/**/*.erb', base: FIXTURES).sort
  end

  def sorbet(*args, env: {})
    Open3.capture3({ 'SORBET_SILENCE_DEV_MESSAGE' => '1' }.merge(env), @sorbet, '--no-config', '--enable-plugin=erb_views',
                   '--allowed-extension=.rb,.rbi,.erb', '--max-threads=1', *args, chdir: FIXTURES)
  end

  def test_template_ruby_matches_the_plugin
    _out, err, _status = sorbet('--stop-after=parser', *@templates, env: { 'SORBET_ERB_VIEWS_PRINT' => '1' })
    printed = err.b.scan(/erb_views: (\S+)\n(.*?)\0/m).to_h
    @templates.each do |template|
      assert_equal printed.fetch(template.b), ErbViews::TemplateRuby.call(File.read(File.join(FIXTURES, template))).b, template
    end
  end

  def test_class_names_match_the_plugin
    out, _err, _status = sorbet('--print=symbol-table', '--quiet', 'stubs.rbi', *@templates)
    @templates.each do |template|
      relative, component = ErbViews::Names.template_path(template)
      next if relative.nil?

      name = "::SorbetViews::#{ErbViews::Names.class_parts(relative, component).join('::')}"
      assert_includes out, "class #{name} < ", template
    end
  end
end
