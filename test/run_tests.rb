# frozen_string_literal: true

# Runs each plugin's fixtures through a Sorbet build that includes the plugins, and checks the errors.
#
#   ruby test/run_tests.rb path/to/sorbet [plugin ...]
#
# A fixture is `<plugin>/test/**/*.rb` or `*.erb` (paths matter to the rspec and erb_views plugins), checked together with the `.rbi` stubs in the same directory, with
# `--enable-plugin=<plugin>`. Mark each expected error on its line with `# error: <part of the message>` (several
# per line are fine; in a template, `<%# error: ... %>`). Any error without a marker, and any marker without an error,
# fails the fixture. A `# sorbet-args: ...` line adds flags (more plugins, `--enable-experimental-rspec`). Each fixture
# is checked on its own, except for the plugins in CHECKED_TOGETHER, whose fixtures depend on each other and are
# checked in one run, as an app's files are. Then it runs each plugin's generator tests, if it has any.

require 'open3'
require 'shellwords'

sorbet = ARGV.shift or abort 'usage: ruby test/run_tests.rb path/to/sorbet [plugin ...]'
sorbet = File.expand_path(sorbet)
root = File.expand_path('..', __dir__)
plugins = ARGV.empty? ? Dir.children(root).select { |dir| Dir.exist?(File.join(root, dir, 'test')) }.sort : ARGV

# erb_views: a view's `render 'templates/card'` calls the method that the partial's own file defines.
CHECKED_TOGETHER = %w[erb_views].freeze

ERROR_LINE = /\A(?<file>[^:\s]+):(?<line>\d+): (?<message>.*?)(?: https:\/\/srb\.help\/\d+)?\z/

failures = 0
fixtures = 0
plugins.each do |plugin|
  test_dir = File.join(root, plugin, 'test')
  stubs = Dir.glob(File.join(test_dir, '*.rbi')).sort
  all = Dir.glob(File.join(test_dir, '**', '*.{rb,erb}')).sort
  groups = CHECKED_TOGETHER.include?(plugin) ? [all] : all.map { |fixture| [fixture] }
  groups.each do |group|
    sources = group.to_h { |fixture| [fixture, File.read(fixture)] }
    extra = sources.values.flat_map { |source| source[/^# sorbet-args: (.*)$/, 1].to_s.shellsplit }.uniq
    command = [sorbet, '--no-config', "--enable-plugin=#{plugin}", *extra, *group, *stubs]
    output, status = Open3.capture2e({ 'SORBET_SILENCE_DEV_MESSAGE' => '1' }, *command)
    # Sorbet exits 100 when it reports errors.
    raise "Sorbet crashed on #{group.join(' ')} (exit #{status.exitstatus}):\n#{output}" unless [0, 100].include?(status.exitstatus)

    # file => line => messages
    reported = Hash.new { |hash, file| hash[file] = Hash.new { |lines, line| lines[line] = [] } }
    output.each_line do |line|
      match = ERROR_LINE.match(line.chomp) or next

      reported[File.expand_path(match[:file])][match[:line].to_i] << match[:message]
    end

    group.each do |fixture|
      fixtures += 1
      expected = {}
      sources.fetch(fixture).each_line.with_index(1) do |line, number|
        messages = line.scan(/# error: ((?:(?!# error:)(?!-?%>).)+)/).flatten.map(&:strip)
        expected[number] = messages unless messages.empty?
      end
      actual = reported[fixture]

      problems = []
      (expected.keys | actual.keys).sort.each do |number|
        remaining = actual[number].dup
        expected.fetch(number, []).each do |want|
          index = remaining.index { |message| message.include?(want) }
          if index
            remaining.delete_at(index)
          else
            problems << "  line #{number}: expected an error containing #{want.inspect}"
          end
        end
        remaining.each { |message| problems << "  line #{number}: unexpected error: #{message}" }
      end

      name = fixture.delete_prefix("#{root}/")
      if problems.empty?
        puts "ok   #{name}"
      else
        failures += 1
        puts "FAIL #{name}", problems
      end
    end

    # An error in a stub (or anything else outside the fixtures) is a mistake in the test itself.
    (reported.keys - group).sort.each do |file|
      failures += 1
      puts "FAIL #{file.delete_prefix("#{root}/")}", reported[file].sort.flat_map { |number, messages| messages.map { |message| "  line #{number}: #{message}" } }
    end
  end
end

puts "#{fixtures} fixtures, #{failures} failed"

# A generator's own tests (`<plugin>/generator/test/*_test.rb`), for the parts that don't need an app. SORBET is the
# binary, for tests that compare the generator with the plugin.
test_failures = plugins.flat_map { |plugin| Dir.glob(File.join(root, plugin, 'generator', 'test', '*_test.rb')).sort }.count do |test|
  puts "Running #{test.delete_prefix("#{root}/")}"
  !system({ 'SORBET' => sorbet }, RbConfig.ruby, test)
end
exit(failures.zero? && test_failures.zero? ? 0 : 1)
