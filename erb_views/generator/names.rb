# frozen_string_literal: true

module ErbViews
  # The names the plugin gives templates, their superclasses and partials' methods: `classPath`, `camelize`,
  # `mailerView`, `partialName` and `partialMethod` in ErbViews.cc. Keep the two in step: generator/test/
  # generator_test.rb checks the class and superclass names against the plugin's.
  module Names
    VIEWS_DIR = 'app/views/'
    COMPONENTS_DIR = 'app/components/'

    module_function

    # Split on anything but ASCII letters and digits, upcase each part's first letter.
    def camelize(name) = name.split(/[^a-zA-Z0-9]+/).reject(&:empty?).map { |part| part[0].upcase + part[1..] }.join

    # The part of `path` after `app/views/` or `app/components/` (at its start or after a `/`), and whether it's a
    # component's template. nil for any other file.
    def template_path(path)
      return nil unless path.end_with?('.erb')

      [[VIEWS_DIR, false], [COMPONENTS_DIR, true]].each do |dir, component|
        at = 0
        while (at = path.index(dir, at))
          return [path[(at + dir.size)..], component] if at.zero? || path[at - 1] == '/'

          at += 1
        end
      end
      nil
    end

    # The constant path of a template's class, without `SorbetViews` (or `SorbetViews::Contexts`):
    # `templates/_row.html.erb` => ["Templates", "RowPartialHtml"].
    def class_parts(relative, component)
      parts = component ? ['Components'] : []
      *dirs, file = relative.split('/')
      parts.concat(dirs.map { |dir| camelize(dir) })
      base, *formats = file.split('.')
      formats.pop # erb
      name = camelize(base)
      name += 'Partial' if base.start_with?('_')
      formats.each { |format| name += camelize(format) }
      parts << name
      parts.map { |part| part.empty? || part.match?(/\A\d/) ? "N#{part}" : part }
    end

    # The method a view's `render 'templates/row'` calls: `__partial__templates__row`. nil for a name the plugin
    # leaves as a render (anything but ASCII letters, digits, `_` and inner single slashes). `partialMethod`.
    def partial_method(name)
      return nil unless name.match?(%r{\A[A-Za-z0-9_]+(/[A-Za-z0-9_]+)*\z})

      "__partial__#{name.gsub('/', '__')}"
    end

    # Whether a view gets the mailers' context: one under a directory named `mailer` or `*_mailer`
    # (`account_mailer/`, `devise/mailer/`), or a layout with `mailer` in its name. `mailerView`.
    def mailer_view?(relative)
      *dirs, file = relative.split('/')
      dirs.any? { |dir| dir == 'mailer' || dir.end_with?('_mailer') } || (dirs.first == 'layouts' && file.include?('mailer'))
    end

    # The directory a view's `render 'row'` looks in: its own (`templates/show.html.erb` => `templates`), empty at
    # the top. `viewDir`.
    def view_dir(relative) = relative.include?('/') ? relative[0...relative.rindex('/')] : ''

    # A partial's name as renders write it: `templates/_row.html.erb` => `templates/row`; nil for a template that
    # isn't a partial. `partialName`.
    def partial_name(relative)
      dir = view_dir(relative)
      file = dir.empty? ? relative : relative[(dir.size + 1)..]
      return nil unless file.start_with?('_')

      base = file[1..].split('.', 2).first
      dir.empty? ? base : "#{dir}/#{base}"
    end

    # What a literal `render 'row'` in a view names: a name without a `/` is in the view's directory.
    def resolve_partial(name, relative)
      dir = view_dir(relative)
      name.include?('/') || dir.empty? ? name : "#{dir}/#{name}"
    end

    # The class a template's class inherits: `::SorbetViews::Contexts::Controller` or `::SorbetViews::Contexts::Mailer`
    # for a view, `::SorbetViews::Contexts::Components::<path>` for a component's template. nil for another file.
    def superclass_name(path)
      relative, component = template_path(path)
      return nil if relative.nil?
      return "::SorbetViews::Contexts::#{class_parts(relative, true).join('::')}" if component

      "::SorbetViews::Contexts::#{mailer_view?(relative) ? 'Mailer' : 'Controller'}"
    end

    # `::SorbetViews::Templates::ShowHtml` for app/views/templates/show.html.erb, or nil.
    def class_name(path)
      relative, component = template_path(path)
      relative && "::SorbetViews::#{class_parts(relative, component).join('::')}"
    end
  end
end
