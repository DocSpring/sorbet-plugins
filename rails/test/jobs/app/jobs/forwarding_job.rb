# typed: true
class ForwardingJob # error: rails plugin: can't copy the parameters of ForwardingJob#perform
  include Sidekiq::Job

  def perform(...); end
end
