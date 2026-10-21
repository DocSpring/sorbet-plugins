# frozen_string_literal: true

# Runs each plugin's fixtures through a Sorbet build that includes the plugins, and checks the errors.
#
#   ruby test/run_tests.rb path/to/sorbet [plugin ...]
#
# A fixture is `<plugin>/test/**/*.rb` (paths matter to the rspec plugin), checked together with the `.rbi` stubs in the same directory, with
# `--enable-plugin=<plugin>`. Mark each expected error on its line with `# error: <part of the message>` (several
# per line are fine). Any error without a marker, and any marker without an error, fails the fixture. A
# `# sorbet-args: ...` line adds flags (more plugins, `--enable-experimental-rspec`).

require 'open3'
require 'shellwords'

sorbet = ARGV.shift or abort 'usage: ruby test/run_tests.rb path/to/sorbet [plugin ...]'
sorbet = File.expand_path(sorbet)
root = File.expand_path('..', __dir__)
plugins = ARGV.empty? ? Dir.children(root).select { |dir| Dir.exist?(File.join(root, dir, 'test')) }.sort : ARGV

ERROR_LINE = /\A(?<file>[^:\s]+):(?<line>\d+): (?<message>.*?)(?: https:\/\/srb\.help\/\d+)?\z/

failures = 0
fixtures = 0
plugins.each do |plugin|
  test_dir = File.join(root, plugin, 'test')
  stubs = Dir.glob(File.join(test_dir, '*.rbi')).sort
  Dir.glob(File.join(test_dir, '**', '*.rb')).sort.each do |fixture|
    fixtures += 1
    source = File.read(fixture)
    extra = source[/^# sorbet-args: (.*)$/, 1].to_s.shellsplit
    expected = {}
    source.each_line.with_index(1) do |line, number|
      messages = line.scan(/# error: ((?:(?!# error:).)+)/).flatten.map(&:strip)
      expected[number] = messages unless messages.empty?
    end

    command = [sorbet, '--no-config', "--enable-plugin=#{plugin}", *extra, fixture, *stubs]
    output, status = Open3.capture2e({ 'SORBET_SILENCE_DEV_MESSAGE' => '1' }, *command)
    # Sorbet exits 100 when it reports errors.
    raise "Sorbet crashed on #{fixture} (exit #{status.exitstatus}):\n#{output}" unless [0, 100].include?(status.exitstatus)

    actual = Hash.new { |hash, key| hash[key] = [] }
    output.each_line do |line|
      match = ERROR_LINE.match(line.chomp) or next
      next unless File.expand_path(match[:file]) == fixture

      actual[match[:line].to_i] << match[:message]
    end

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
end

puts "#{fixtures} fixtures, #{failures} failed"
exit(failures.zero? ? 0 : 1)
