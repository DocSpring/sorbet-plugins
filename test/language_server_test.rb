# frozen_string_literal: true

# Checks that the plugins keep working in Sorbet's language server after edits. The server indexes an edited file,
# and hashes it to choose the fast or slow path, with copies of its global state, and every copy has to run the
# plugins: when the indexer's didn't, the first edit to a job made every job's `perform_async` Sidekiq's untyped one,
# and when the hashing state didn't, an edit to a file a plugin rewrites never preempted a slow path. The app is the
# rails plugin's `jobs` case, plus a caller that passes a String where IntegrationUpload#perform's sig says Integer.
# Half-typed code mustn't stop the server either.
#
#   ruby test/language_server_test.rb path/to/sorbet

require 'fileutils'
require 'json'
require 'tmpdir'

sorbet = File.expand_path(ARGV.fetch(0) { abort 'usage: ruby test/language_server_test.rb path/to/sorbet' })
root = File.expand_path('..', __dir__)
# Every slow path waits this long (`--sleep-in-slow-path`), so an edit can arrive while one runs.
SLOW_PATH_SLEEP = 2
# Seconds without a message from the server, while it reports no slow path running and has answered the request sent
# after the last edit: it has finished checking.
QUIET = 1.5

class LanguageServer
  def initialize(sorbet, dir, log)
    @dir = dir
    @io = IO.popen({ 'SORBET_SILENCE_DEV_MESSAGE' => '1' },
                   [sorbet, '--lsp', '--disable-watchman', '--no-config', '--enable-plugin=rails', '-v',
                    "--debug-log-file=#{log}", "--sleep-in-slow-path=#{SLOW_PATH_SLEEP}", '--dir', '.'],
                   'r+', chdir: dir, err: File::NULL)
    @messages = Queue.new
    @reader = Thread.new { read_messages }
    @diagnostics = {}
    @versions = {}
    @operations = [] # the slow paths (and other long operations) the server says are running
    @answered = [] # the ids of the requests it has answered
    @next_id = 1
    initialize_id = request('initialize', processId: Process.pid, rootUri: uri('.'), capabilities: {},
                                          initializationOptions: { supportsOperationNotifications: true })
    notify('initialized', {})
    settle(initialize_id)
  end

  # The messages for each file with errors, by path relative to the app.
  def errors
    @diagnostics.reject { |_path, messages| messages.empty? }
  end

  # An editor change to the file's buffer, opening it first. Waits for the server to finish checking unless `wait` is
  # false: it answers a request sent after the change only once it has checked the change, so a late result can't
  # arrive after the wait.
  def edit(relative, text, wait: true)
    unless @versions.key?(relative)
      @versions[relative] = 1
      notify('textDocument/didOpen', textDocument: { uri: uri(relative), languageId: 'ruby', version: 1,
                                                     text: File.read(File.join(@dir, relative)) })
    end
    @versions[relative] += 1
    notify('textDocument/didChange', textDocument: { uri: uri(relative), version: @versions[relative] },
                                     contentChanges: [{ text: text }])
    settle(request('textDocument/hover', textDocument: { uri: uri(relative) }, position: { line: 0, character: 0 })) if wait
  end

  # Waits until the server says a long operation (a slow path) has started.
  def wait_for_operation
    deadline = Time.now + 60
    until @operations.any?
      left = deadline - Time.now
      raise 'the language server started no slow path within a minute' if left <= 0

      message = @messages.pop(timeout: left)
      raise 'the language server stopped' if message.nil? && !alive?

      handle(message) if message
    end
  end

  # Whether the server is still running: the reader stops when its output ends.
  def alive?
    @reader.alive? || !@messages.empty?
  end

  def close
    @io.close
    @reader.join
  end

  private

  def uri(relative)
    "file://#{File.expand_path(relative, @dir)}"
  end

  def notify(method, params)
    write(jsonrpc: '2.0', method: method, params: params)
  end

  # Sends a request and returns its id.
  def request(method, params)
    id = @next_id
    @next_id += 1
    write(jsonrpc: '2.0', id: id, method: method, params: params)
    id
  end

  def write(message)
    body = JSON.generate(message)
    @io.write("Content-Length: #{body.bytesize}\r\n\r\n#{body}")
    @io.flush
  end

  def read_messages
    loop do
      length = nil
      while (line = @io.gets)
        line = line.strip
        break if line.empty?

        length = Integer(line.split(':', 2).last.strip) if line.downcase.start_with?('content-length:')
      end
      break if line.nil? || length.nil?

      @messages << JSON.parse(@io.read(length))
    end
  rescue IOError
    # The test closed the server: nothing more to read.
  end

  # Waits for the answer to request `id`, then until no operation runs and the server has been quiet for QUIET seconds.
  def settle(id)
    deadline = Time.now + 120
    loop do
      left = deadline - Time.now
      raise "the language server was still busy after two minutes (running: #{@operations.inspect})" if left <= 0

      message = @messages.pop(timeout: [QUIET, left].min)
      if message
        handle(message)
      elsif !alive?
        raise 'the language server stopped'
      elsif @operations.empty? && @answered.include?(id)
        return
      end
    end
  end

  def handle(message)
    raise 'the language server stopped' if message.nil?

    @answered << message['id'] if message.key?('id') && !message.key?('method')
    case message['method']
    when 'sorbet/showOperation'
      params = message['params']
      if params['status'] == 'start'
        @operations << params['operationName']
      else
        @operations.delete(params['operationName'])
      end
    when 'textDocument/publishDiagnostics'
      path = message['params']['uri'].delete_prefix("file://#{File.realpath(@dir)}/").delete_prefix("file://#{@dir}/")
      @diagnostics[path] = message['params']['diagnostics'].map { |diagnostic| diagnostic['message'] }
    end
  end
end

failures = []
check = lambda do |description, ok, detail = nil|
  puts "#{ok ? 'ok  ' : 'FAIL'} #{description}"
  failures << description unless ok
  puts detail if !ok && detail
end

Dir.mktmpdir do |dir|
  FileUtils.cp_r(Dir.glob(File.join(root, 'rails/test/jobs/*')), dir)
  FileUtils.cp(Dir.glob(File.join(root, 'rails/test/*.rbi')), dir)
  File.write(File.join(dir, 'app/services/caller.rb'),
             "# typed: true\nBoxUploadJob.perform_async('Submission', 'one')\n")
  log = File.join(dir, 'lsp.log')
  module_file = File.join(dir, 'app/jobs/concerns/integration_upload.rb')
  # The plugin writes BoxUploadJob's `perform_async` with a module of type aliases for IntegrationUpload#perform's
  # parameter types, so an edit's hash covers the plugin's output only if the plugin ran when it was hashed.
  job = 'app/jobs/box_upload_job.rb'
  job_text = File.read(File.join(dir, job))
  caller_file = 'app/services/caller.rb'
  caller_text = File.read(File.join(dir, caller_file))
  # The log the server writes during the block.
  logged = lambda do |&block|
    from = File.size(log)
    block.call
    File.read(log)[from..]
  end
  slow_reasons = ->(text) { text.scan(/Taking slow path because .*/) }

  server = LanguageServer.new(sorbet, dir, log)
  begin
    start = server.errors
    caller_error = 'Expected `Integer` but found `String("one")` for argument `action_id`'
    check.call('the caller has its error at start', start[caller_file] == [caller_error], "  errors: #{start.inspect}")
    simple = start['app/jobs/simple_job.rb']
    check.call('SimpleJob has its typed perform_async at start', simple&.include?('Revealed type: `String`'),
               "  simple_job.rb: #{simple.inspect}")

    text = logged.call { server.edit(job, "#{job_text}\n") }
    check.call('a blank line in a job changes no file\'s errors', server.errors == start,
               "  before: #{start.inspect}\n  after:  #{server.errors.inspect}")
    check.call('a blank line in a job takes the fast path', slow_reasons.(text).empty?,
               "  #{slow_reasons.(text).join("\n  ")}")

    others = ->(errors) { errors.except(caller_file) }
    text = logged.call { server.edit(caller_file, "#{caller_text}class UnrelatedClass; end\n") }
    check.call('a new class takes the slow path', !slow_reasons.(text).empty?)
    check.call('the slow path changes no other file\'s errors', others.(server.errors) == others.(start),
               "  before: #{others.(start).inspect}\n  after:  #{others.(server.errors).inspect}")

    # Removing the class takes the slow path again, and a blank line in the job, sent while it runs, preempts it.
    text = logged.call do
      server.edit(caller_file, caller_text, wait: false)
      server.wait_for_operation
      server.edit(job, "#{job_text}\n\n")
    end
    check.call('a blank line in a job, sent during a slow path, preempts it', text.include?('Preempting slow path'),
               "  #{slow_reasons.(text).join("\n  ")}")
    check.call('the preempted slow path changes no file\'s errors', server.errors == start,
               "  before: #{start.inspect}\n  after:  #{server.errors.inspect}")

    # Half-typed code: Prism leaves a name after `::` unset, which crashed the plugin (and the server) on this keystroke.
    server.edit(caller_file, "#{caller_text}class TypingJob < BoxUploadJob::\n")
    check.call('a constant path with nothing after `::` leaves the server running',
               server.errors[caller_file]&.any? { |message| message.include?('expected constant name following "::"') },
               "  #{caller_file}: #{server.errors[caller_file].inspect}")
    server.edit(caller_file, caller_text)
    check.call('the file has its errors back once the constant is finished', server.errors == start,
               "  before: #{start.inspect}\n  after:  #{server.errors.inspect}")

    File.write(module_file, File.read(module_file).sub('action_id: Integer', 'action_id: String'))
    server.edit(job, "#{job_text}\n\n\n")
    # In the job's own file, `perform_in(60, 'Submission', 'one')` passes now and `perform_async('Submission', 1)`
    # doesn't.
    job_errors = ['Expected `String` but found `Integer(1)` for argument `action_id`', 'Revealed type: `String`']
    check.call('the job typed from the saved sig once it is edited',
               !server.errors.key?(caller_file) && server.errors[job]&.sort == job_errors.sort,
               "  #{caller_file}: #{server.errors[caller_file].inspect}\n  #{job}: #{server.errors[job].inspect}")
  ensure
    server.close
  end
end

puts failures.empty? ? 'Language server: ok' : "Language server: #{failures.size} failed"
exit(failures.empty? ? 0 : 1)
