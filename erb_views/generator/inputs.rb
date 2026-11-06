# frozen_string_literal: true

require 'digest'
require 'open3'

module ErbViews
  # What the generated RBIs depend on, as one digest: the app's Ruby, RBI and ERB files (git's tracked and untracked
  # ones, but not spec/, which the views' types don't depend on), so the RBIs themselves too, and a hand edit gets
  # rewritten; Gemfile.lock; sorbet/config, whose flags and ignored paths change what Sorbet reports; the generator's
  # own code; and the Sorbet binary. Plain Ruby, so a script can check it before booting the app:
  # bin/sorbet-plugin-rbis skips the generator when the digest is the one its last run left.
  module Inputs
    module_function

    # The Sorbet binary the generator type-checks with: SRB_SORBET_EXE (as for `srb`), or the sorbet-static gem's.
    def sorbet
      path = ENV.fetch('SRB_SORBET_EXE', '')
      path = File.join(Gem::Specification.find_by_name('sorbet-static').gem_dir, 'libexec', 'sorbet') if path.empty?
      raise "no Sorbet binary at #{path}" unless File.executable?(path)

      path
    end

    def digest(root, sorbet: self.sorbet)
      listed, status = Open3.capture2('git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard', '--',
                                      '*.rb', '*.rbi', '*.erb', 'Gemfile.lock', 'sorbet/config', ':!:spec/',
                                      chdir: root.to_s)
      raise "git ls-files failed in #{root}" unless status.success?

      digest = Digest::SHA256.new
      listed.split("\0").sort.each do |file|
        path = File.join(root.to_s, file)
        digest << file << "\0" << File.binread(path) << "\0" if File.file?(path) # not one deleted but still staged
      end
      Dir.glob('**/*.rb', base: File.expand_path('..', __dir__)).sort.each do |file|
        digest << file << "\0" << File.binread(File.expand_path("../#{file}", __dir__)) << "\0"
      end
      stat = File.stat(sorbet)
      digest << "#{File.realpath(sorbet)} #{stat.size} #{stat.mtime.to_i}"
      digest.hexdigest
    end
  end
end
