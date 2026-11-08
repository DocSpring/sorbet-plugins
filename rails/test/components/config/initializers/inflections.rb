# typed: true
ActiveSupport::Inflector.inflections(:en) do |inflect|
  inflect.acronym 'OAuth'
  inflect.irregular 'cactus', 'cacti'
  inflect.uncountable %w[gear]
end
