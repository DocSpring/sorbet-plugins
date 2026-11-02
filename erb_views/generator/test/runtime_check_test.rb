# frozen_string_literal: true

# Tests for the check that the test environment runs on each render (runtime_check.rb), against a stand-in for
# ActionView's templates and an app with a view, a partial and their RBIs.
#
#   ruby erb_views/generator/test/runtime_check_test.rb

ENV['MT_NO_PLUGINS'] = '1' # other gems' Minitest plugins can load conflicting dependencies
require 'minitest/autorun'
require 'fileutils'
require 'sorbet-runtime'
require 'tmpdir'

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

  FILES = {
    'app/views/records/show.html.erb' => "<%= @record %><%= @note %><% @title = 'x' %>\n",
    'sorbet/rbi/erb_views/app/views/records/show.html.erb.rbi' => <<~RBI,
      class ::SorbetViews::Records::ShowHtml
        sig { void }
        def initialize
          @note = T.let(T.unsafe(nil), T.nilable(::String))
          @record = T.let(T.unsafe(nil), ::Record)
          @title = T.let(T.unsafe(nil), ::String)
          @anything = T.let(T.unsafe(nil), T.untyped)
        end
      end
    RBI
    'app/views/records/_row.html.erb' => <<~ERB,
      <%# locals: (record:, records: [], missing: nil, other: nil, free: nil) -%>
      <%# sig { params(record: Record, records: T::Array[Record], missing: Record::PrivateRelation, other: Nonexistent) } -%>
    ERB
    'app/views/records/_loose.html.erb' => "<%= record %>\n",
    'sorbet/rbi/erb_views/app/views/records/_loose.html.erb.rbi' => <<~RBI,
      class ::SorbetViews::Records::LoosePartialHtml
        sig { returns(::Record) }
        def record; end
      end
    RBI
  }.freeze

  def setup
    @root = Dir.mktmpdir('runtime_check_test')
    FILES.each do |path, text|
      FileUtils.mkdir_p(File.dirname(File.join(@root, path)))
      File.write(File.join(@root, path), text)
    end
    ErbViews::RuntimeCheck.root = @root
    ErbViews::RuntimeCheck.rbi_dir = File.join(@root, 'sorbet/rbi/erb_views')
    ErbViews::RuntimeCheck.instance_variable_set(:@types, nil)
  end

  def teardown = FileUtils.rm_rf(@root)

  def template(path) = ActionView::Template.new(File.join(@root, path))
  def show = template('app/views/records/show.html.erb')
  def row = template('app/views/records/_row.html.erb')

  def test_passes_values_of_the_declared_types
    result = show.render(View.new({ :@record => Record.new, :@note => nil }), {})
    assert_equal [:rendered, {}, []], result
  end

  def test_raises_for_an_ivar_of_another_type
    error = assert_raises(ErbViews::RuntimeCheck::TypeError) { show.render(View.new({ :@record => 'record' }), {}) }
    assert_match %r{\Aapp/views/records/show\.html\.erb rendered with @record = String, which isn't a Record, as sorbet/rbi/erb_views/app/views/records/show\.html\.erb\.rbi declares},
                 error.message
  end

  def test_raises_for_an_unset_ivar_that_isnt_nilable
    error = assert_raises(ErbViews::RuntimeCheck::TypeError) { show.render(View.new({}), {}) }
    assert_match(/@record = NilClass/, error.message)
  end

  def test_an_ivar_the_template_assigns_may_be_unset
    assert_equal :rendered, show.render(View.new({ :@record => Record.new }), {}).first
    assert_raises(ErbViews::RuntimeCheck::TypeError) { show.render(View.new({ :@record => Record.new, :@title => 1 }), {}) }
  end

  def test_checks_only_the_locals_a_render_passes
    assert_equal :rendered, row.render(View.new({}), { record: Record.new }, implicit_locals: [:x]).first
    error = assert_raises(ErbViews::RuntimeCheck::TypeError) { row.render(View.new({}), { record: 1 }) }
    assert_match(/record = Integer, which isn't a Record, as its sig comment declares/, error.message)
  end

  def test_reads_the_locals_of_a_partial_without_a_locals_comment_from_its_rbi
    loose = template('app/views/records/_loose.html.erb')
    assert_equal :rendered, loose.render(View.new({}), { record: Record.new }).first
    assert_raises(ErbViews::RuntimeCheck::TypeError) { loose.render(View.new({}), { record: 1 }) }
  end

  def test_checks_a_generic_by_its_class_only
    assert_equal :rendered, row.render(View.new({}), { record: Record.new, records: ['not a record'] }).first
    assert_raises(ErbViews::RuntimeCheck::TypeError) { row.render(View.new({}), { records: Record.new }) }
  end

  def test_skips_types_whose_constants_exist_only_in_rbi
    assert_equal :rendered, row.render(View.new({}), { missing: 1 }).first
  end

  def test_raises_for_a_constant_that_doesnt_exist
    assert_raises(NameError) { row.render(View.new({}), { other: 1 }) }
  end

  def test_checks_only_locals_in_a_view_spec
    view = Struct.new(:controller).new(ActionView::TestCase::TestController.new)
    assert_equal :rendered, show.render(view, {}).first
    assert_raises(ErbViews::RuntimeCheck::TypeError) { row.render(view, { record: 1 }) }
  end

  def test_skips_templates_outside_the_apps_views
    gem_template = ActionView::Template.new('/gems/kaminari/app/views/kaminari/_page.html.erb')
    assert_equal :rendered, gem_template.render(View.new({ :@record => 1 }), { page: 1 }).first
  end

  def test_a_view_without_an_rbi_has_no_ivar_types
    path = 'app/views/records/new.html.erb'
    File.write(File.join(@root, path), "<%= @record %>\n")
    assert_equal :rendered, template(path).render(View.new({ :@record => 1 }), {}).first
  end
end
