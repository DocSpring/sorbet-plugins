# frozen_string_literal: true

# Tests for the generator's parts that don't need a Rails app: reading Sorbet's types, and the code facts the render
# graph and the type probe use. The rest of the generator runs against the app it generates for.
#
#   ruby erb_views/generator/test/generator_test.rb

ENV['MT_NO_PLUGINS'] = '1' # other gems' Minitest plugins can load conflicting dependencies
require 'minitest/autorun'
require 'set'
require_relative '../code'
require_relative '../sorbet_type'

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
end
