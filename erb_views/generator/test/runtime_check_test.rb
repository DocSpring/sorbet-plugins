# frozen_string_literal: true

# Tests for the check that the test environment runs on each render (runtime_check.rb), against a stand-in for
# ActionView's templates. The generated file the app loads holds the same code, with its types.
#
#   ruby erb_views/generator/test/runtime_check_test.rb

ENV['MT_NO_PLUGINS'] = '1' # other gems' Minitest plugins can load conflicting dependencies
require 'minitest/autorun'
require 'pathname'
require 'sorbet-runtime'

module Rails
  def self.root = Pathname.new('/app')
end

module ActionView
  class Template
    attr_reader :identifier

    def initialize(identifier) = @identifier = identifier

    def render(_view, locals, _buffer = nil, implicit_locals: []) = [:rendered, locals, implicit_locals]
  end

  class TestCase
    class TestController; end
  end
end

class Record; end

require_relative '../runtime_check'

class RuntimeCheckTest < Minitest::Test
  View = Struct.new(:ivars) do
    def instance_variable_get(name) = ivars[name]
  end

  def setup
    ErbViews::RuntimeCheck.types = {
      'app/views/records/show.html.erb' => {
        ivars: { :@record => -> { Record }, :@note => -> { T.nilable(String) } },
        locals: {},
      },
      'app/views/records/_row.html.erb' => {
        ivars: {},
        locals: { record: -> { Record }, records: -> { T::Array[Record] }, missing: -> { Record::PrivateRelation } },
      },
    }
  end

  def show = ActionView::Template.new('/app/app/views/records/show.html.erb')
  def row = ActionView::Template.new('/app/app/views/records/_row.html.erb')

  def test_passes_values_of_the_declared_types
    result = show.render(View.new({ :@record => Record.new, :@note => nil }), {})
    assert_equal [:rendered, {}, []], result
  end

  def test_raises_for_an_ivar_of_another_type
    error = assert_raises(ErbViews::RuntimeCheck::TypeError) { show.render(View.new({ :@record => 'record' }), {}) }
    assert_match %r{\Aapp/views/records/show\.html\.erb rendered with @record = String, which isn't a Record}, error.message
  end

  def test_raises_for_an_unset_ivar_that_isnt_nilable
    error = assert_raises(ErbViews::RuntimeCheck::TypeError) { show.render(View.new({}), {}) }
    assert_match(/@record = NilClass/, error.message)
  end

  def test_checks_only_the_locals_a_render_passes
    assert_equal :rendered, row.render(View.new({}), { record: Record.new }, implicit_locals: [:x]).first
    assert_raises(ErbViews::RuntimeCheck::TypeError) { row.render(View.new({}), { record: 1 }) }
  end

  def test_checks_a_generic_by_its_class_only
    assert_equal :rendered, row.render(View.new({}), { records: ['not a record'] }).first
    assert_raises(ErbViews::RuntimeCheck::TypeError) { row.render(View.new({}), { records: Record.new }) }
  end

  def test_skips_types_whose_constants_exist_only_in_rbi
    assert_equal :rendered, row.render(View.new({}), { missing: 1 }).first
  end

  def test_raises_for_a_constant_that_doesnt_exist
    ErbViews::RuntimeCheck.types['app/views/records/_row.html.erb'][:locals][:other] = -> { Nonexistent }
    assert_raises(NameError) { row.render(View.new({}), { other: 1 }) }
  end

  def test_checks_only_locals_in_a_view_spec
    view = Struct.new(:controller).new(ActionView::TestCase::TestController.new)
    assert_equal :rendered, show.render(view, {}).first
    assert_raises(ErbViews::RuntimeCheck::TypeError) { row.render(view, { record: 1 }) }
  end

  def test_skips_templates_without_types
    template = ActionView::Template.new('/gems/kaminari/app/views/kaminari/_page.html.erb')
    assert_equal :rendered, template.render(View.new({ :@record => 1 }), { page: 1 }).first
  end
end
