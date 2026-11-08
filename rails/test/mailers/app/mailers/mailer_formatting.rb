# typed: true
# A class in app/mailers that isn't a mailer gets no actions.
class MailerFormatting
  def render_text(text); end
end

MailerFormatting.render_text('x') # error: Method `render_text` does not exist on `T.class_of(MailerFormatting)`
