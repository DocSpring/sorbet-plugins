# frozen_string_literal: true

# Checks the plugin's Ruby for every template in an app against Herb's (https://herb-tools.dev), which parses ERB as
# Rails does, and against the generator's copy of the transform (generator/template_ruby.rb), and that Sorbet parses
# all of it. Run it with the herb gem installed:
#
#   gem install herb
#   ruby erb_views/scripts/conformance.rb path/to/sorbet path/to/app
#
# It prints each template whose Ruby differs from `Herb.extract_ruby`'s or the generator's, with the first line that
# differs, and each syntax error, and exits 1 if there are any.

require 'herb'
require 'open3'
require_relative '../generator/template_ruby'

sorbet = File.expand_path(ARGV.fetch(0) { abort 'usage: ruby erb_views/scripts/conformance.rb path/to/sorbet path/to/app' })
app = File.expand_path(ARGV.fetch(1) { abort 'usage: ruby erb_views/scripts/conformance.rb path/to/sorbet path/to/app' })
templates = Dir.glob(File.join(app, '{app/views,app/components}', '**', '*.erb')).sort
abort "No templates under #{app}/app/views or app/components" if templates.empty?

# One thread, so the templates' printed Ruby doesn't interleave on stderr.
command = [sorbet, '--no-config', '--enable-plugin=erb_views', '--stop-after=parser', '--max-threads=1', *templates]
_out, err, status = Open3.capture3({ 'SORBET_ERB_VIEWS_PRINT' => '1', 'SORBET_SILENCE_DEV_MESSAGE' => '1' }, *command)
raise "Sorbet crashed (exit #{status.exitstatus}):\n#{err[-4000..] || err}" unless [0, 100].include?(status.exitstatus)

printed = {}
rest = err.dup
while (start = rest.index('erb_views: '))
  header_end = rest.index("\n", start)
  stop = rest.index("\0", header_end)
  path = rest[(start + 'erb_views: '.length)...header_end]
  printed[File.expand_path(path)] = rest[(header_end + 1)...stop]
  rest = rest[(stop + 1)..]
end
syntax_errors = rest.lines.grep(/https:\/\/srb\.help\/2\d\d\d/)

def first_difference(name, ours, other_name, other)
  ours_lines = ours.split("\n", -1)
  other_lines = other.split("\n", -1)
  line = (0...[ours_lines.size, other_lines.size].max).find { |i| ours_lines[i] != other_lines[i] }
  ["#{name}:#{line + 1}", "  plugin:    #{ours_lines[line].inspect}", "  #{other_name}: #{other_lines[line].inspect}"]
end

differences = 0
templates.each do |template|
  ours = printed[template] or raise "The plugin printed nothing for #{template}"
  source = File.read(template)
  name = template.delete_prefix("#{app}/")
  herb = Herb.extract_ruby(source)
  port = ErbViews::TemplateRuby.call(source)
  problems = []
  problems.concat(first_difference(name, ours, 'herb     ', herb)) unless ours == herb
  problems.concat(first_difference(name, ours, 'generator', port)) unless ours == port
  next if problems.empty?

  differences += 1
  puts problems
end

puts syntax_errors
puts "#{templates.size} templates, #{differences} differ from Herb or the generator, #{syntax_errors.size} syntax errors"
exit(differences.zero? && syntax_errors.empty? ? 0 : 1)
