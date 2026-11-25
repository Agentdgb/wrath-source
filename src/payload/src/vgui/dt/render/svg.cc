#include "../../includes.hh"

namespace dt::svg
{
	namespace
	{
		/* sub scanlines per pixel row. coverage is analytic on x, supersampled on y */
		constexpr int k_sub_samples = 16;

		/* how finely curves get flattened, in canvas units */
		constexpr float k_curve_tolerance = 0.35f;

		using point_t = math::c_vector_2d;

		struct contour_t
		{
			std::vector<point_t> points{};
			bool closed{ false };
		};

		struct element_t
		{
			std::vector<contour_t> contours{};
			bool fill{ false };
			bool stroke{ false };
			bool even_odd{ false };
			float stroke_width{ 1.f };
		};

		struct edge_t
		{
			float x0{}, y0{}, x1{}, y1{};
			int direction{};
		};

		/* --------------------------------------------------------------------- */
		/* attribute scanning                                                     */
		/* --------------------------------------------------------------------- */

		bool is_name_boundary(char character)
		{
			return !(std::isalnum(static_cast<unsigned char>(character)) || character == '-' || character == ':' || character == '_');
		}

		std::optional<std::string> find_attribute(std::string_view source, std::string_view name)
		{
			std::size_t position = 0;

			while ((position = source.find(name, position)) != std::string_view::npos)
			{
				const bool left_ok = (position == 0) || is_name_boundary(source[position - 1]);

				std::size_t cursor = position + name.size();
				while (cursor < source.size() && std::isspace(static_cast<unsigned char>(source[cursor])))
					++cursor;

				if (left_ok && cursor < source.size() && source[cursor] == '=')
				{
					++cursor;
					while (cursor < source.size() && std::isspace(static_cast<unsigned char>(source[cursor])))
						++cursor;

					if (cursor < source.size() && (source[cursor] == '"' || source[cursor] == '\''))
					{
						const char quote = source[cursor];
						const std::size_t start = cursor + 1;
						const std::size_t end = source.find(quote, start);

						if (end != std::string_view::npos)
							return std::string(source.substr(start, end - start));
					}
				}

				position += name.size();
			}

			return std::nullopt;
		}

		float attribute_float(std::string_view attributes, std::string_view name, float fallback)
		{
			const auto raw = find_attribute(attributes, name);
			if (!raw.has_value())
				return fallback;

			// strip units such as px / % so "17px" still parses
			std::string cleaned{};
			for (const char character : *raw)
			{
				if (std::isdigit(static_cast<unsigned char>(character)) || character == '.' || character == '-' || character == '+' ||
					character == 'e' || character == 'E')
					cleaned.push_back(character);
			}

			if (cleaned.empty())
				return fallback;

			try {
				return std::stof(cleaned);
			}
			catch (...) {
				return fallback;
			}
		}

		bool is_paint(const std::optional<std::string>& value)
		{
			return value.has_value() && *value != "none" && *value != "transparent" && !value->empty();
		}

		/* --------------------------------------------------------------------- */
		/* path data                                                              */
		/* --------------------------------------------------------------------- */

		class c_path_builder
		{
		public:
			explicit c_path_builder(std::vector<contour_t>& target) : m_target(target) { }

			void move_to(point_t position)
			{
				this->flush();

				this->m_current.points.push_back(position);
				this->m_cursor = position;
				this->m_start = position;
			}

			void line_to(point_t position)
			{
				if (this->m_current.points.empty())
					this->m_current.points.push_back(this->m_cursor);

				this->m_current.points.push_back(position);
				this->m_cursor = position;
			}

			void cubic_to(point_t control_a, point_t control_b, point_t position)
			{
				if (this->m_current.points.empty())
					this->m_current.points.push_back(this->m_cursor);

				const point_t start = this->m_cursor;
				const int segments = this->segment_count((control_a - start).length() + (control_b - control_a).length() + (position - control_b).length());

				for (int step = 1; step <= segments; ++step)
				{
					const float t = static_cast<float>(step) / static_cast<float>(segments);
					const float inverse = 1.f - t;

					const float w0 = inverse * inverse * inverse;
					const float w1 = 3.f * inverse * inverse * t;
					const float w2 = 3.f * inverse * t * t;
					const float w3 = t * t * t;

					this->m_current.points.push_back(point_t(
						start.x * w0 + control_a.x * w1 + control_b.x * w2 + position.x * w3,
						start.y * w0 + control_a.y * w1 + control_b.y * w2 + position.y * w3
					));
				}

				this->m_cursor = position;
				this->m_last_cubic_control = control_b;
				this->m_had_cubic = true;
				this->m_had_quadratic = false;
			}

			void quadratic_to(point_t control, point_t position)
			{
				// promote to a cubic so a single flattening path stays authoritative
				const point_t start = this->m_cursor;
				const point_t control_a(start.x + (2.f / 3.f) * (control.x - start.x), start.y + (2.f / 3.f) * (control.y - start.y));
				const point_t control_b(position.x + (2.f / 3.f) * (control.x - position.x), position.y + (2.f / 3.f) * (control.y - position.y));

				this->cubic_to(control_a, control_b, position);

				this->m_last_quadratic_control = control;
				this->m_had_quadratic = true;
				this->m_had_cubic = false;
			}

			void close()
			{
				if (!this->m_current.points.empty())
				{
					this->m_current.closed = true;
					this->m_target.push_back(std::move(this->m_current));
					this->m_current = contour_t{};
				}

				this->m_cursor = this->m_start;
			}

			void flush()
			{
				if (this->m_current.points.size() >= 2)
					this->m_target.push_back(std::move(this->m_current));

				this->m_current = contour_t{};
			}

			point_t cursor() const { return this->m_cursor; }

			point_t reflected_cubic_control() const {
				return this->m_had_cubic
					? point_t(2.f * this->m_cursor.x - this->m_last_cubic_control.x, 2.f * this->m_cursor.y - this->m_last_cubic_control.y)
					: this->m_cursor;
			}

			point_t reflected_quadratic_control() const {
				return this->m_had_quadratic
					? point_t(2.f * this->m_cursor.x - this->m_last_quadratic_control.x, 2.f * this->m_cursor.y - this->m_last_quadratic_control.y)
					: this->m_cursor;
			}

			void clear_smooth_state() { this->m_had_cubic = false; this->m_had_quadratic = false; }

		private:
			int segment_count(float control_length) const {
				const int estimate = static_cast<int>(std::ceil(control_length / std::max(0.01f, k_curve_tolerance)));
				return std::clamp(estimate, 4, 96);
			}

			std::vector<contour_t>& m_target;
			contour_t m_current{};

			point_t m_cursor{};
			point_t m_start{};
			point_t m_last_cubic_control{};
			point_t m_last_quadratic_control{};

			bool m_had_cubic{ false };
			bool m_had_quadratic{ false };
		};

		/* @c_number_reader:
			svg path data lets numbers run together ("1-2.5.5"), so it needs its own scanner
		*/
		class c_number_reader
		{
		public:
			explicit c_number_reader(const std::string& data) : m_data(data) { }

			void skip_separators()
			{
				while (this->m_cursor < this->m_data.size() &&
					(std::isspace(static_cast<unsigned char>(this->m_data[this->m_cursor])) || this->m_data[this->m_cursor] == ','))
					++this->m_cursor;
			}

			bool at_end()
			{
				this->skip_separators();
				return this->m_cursor >= this->m_data.size();
			}

			bool peek_command(char& out)
			{
				this->skip_separators();
				if (this->m_cursor >= this->m_data.size())
					return false;

				const char character = this->m_data[this->m_cursor];
				if (std::isalpha(static_cast<unsigned char>(character)))
				{
					out = character;
					return true;
				}

				return false;
			}

			void consume_command() { ++this->m_cursor; }

			bool read(float& out)
			{
				this->skip_separators();
				if (this->m_cursor >= this->m_data.size())
					return false;

				const std::size_t start = this->m_cursor;

				if (this->m_data[this->m_cursor] == '+' || this->m_data[this->m_cursor] == '-')
					++this->m_cursor;

				bool seen_digit = false;
				while (this->m_cursor < this->m_data.size() && std::isdigit(static_cast<unsigned char>(this->m_data[this->m_cursor])))
				{
					++this->m_cursor;
					seen_digit = true;
				}

				if (this->m_cursor < this->m_data.size() && this->m_data[this->m_cursor] == '.')
				{
					++this->m_cursor;
					while (this->m_cursor < this->m_data.size() && std::isdigit(static_cast<unsigned char>(this->m_data[this->m_cursor])))
					{
						++this->m_cursor;
						seen_digit = true;
					}
				}

				if (!seen_digit)
				{
					this->m_cursor = start;
					return false;
				}

				if (this->m_cursor < this->m_data.size() &&
					(this->m_data[this->m_cursor] == 'e' || this->m_data[this->m_cursor] == 'E'))
				{
					const std::size_t exponent_start = this->m_cursor;
					++this->m_cursor;

					if (this->m_cursor < this->m_data.size() && (this->m_data[this->m_cursor] == '+' || this->m_data[this->m_cursor] == '-'))
						++this->m_cursor;

					bool exponent_digits = false;
					while (this->m_cursor < this->m_data.size() && std::isdigit(static_cast<unsigned char>(this->m_data[this->m_cursor])))
					{
						++this->m_cursor;
						exponent_digits = true;
					}

					if (!exponent_digits)
						this->m_cursor = exponent_start;
				}

				out = std::strtof(this->m_data.substr(start, this->m_cursor - start).c_str(), nullptr);
				return true;
			}

			bool read_point(point_t& out)
			{
				float x{}, y{};
				if (!this->read(x) || !this->read(y))
					return false;

				out = point_t(x, y);
				return true;
			}

		private:
			const std::string& m_data;
			std::size_t m_cursor{ 0 };
		};

		void parse_path_data(const std::string& data, std::vector<contour_t>& contours)
		{
			c_path_builder builder(contours);
			c_number_reader reader(data);

			char command = 0;

			while (!reader.at_end())
			{
				char next{};
				if (reader.peek_command(next))
				{
					command = next;
					reader.consume_command();
				}
				else if (command == 0)
				{
					break;
				}
				else if (command == 'M')
				{
					// implicit repetition of a moveto turns into a lineto
					command = 'L';
				}
				else if (command == 'm')
				{
					command = 'l';
				}

				const point_t cursor = builder.cursor();
				const bool relative = std::islower(static_cast<unsigned char>(command)) != 0;
				const point_t origin = relative ? cursor : point_t(0.f, 0.f);

				switch (std::toupper(static_cast<unsigned char>(command)))
				{
				case 'M':
				{
					point_t target{};
					if (!reader.read_point(target)) return;
					builder.move_to(origin + target);
					builder.clear_smooth_state();
					break;
				}
				case 'L':
				{
					point_t target{};
					if (!reader.read_point(target)) return;
					builder.line_to(origin + target);
					builder.clear_smooth_state();
					break;
				}
				case 'H':
				{
					float x{};
					if (!reader.read(x)) return;
					builder.line_to(point_t(origin.x + x, cursor.y));
					builder.clear_smooth_state();
					break;
				}
				case 'V':
				{
					float y{};
					if (!reader.read(y)) return;
					builder.line_to(point_t(cursor.x, origin.y + y));
					builder.clear_smooth_state();
					break;
				}
				case 'C':
				{
					point_t control_a{}, control_b{}, target{};
					if (!reader.read_point(control_a) || !reader.read_point(control_b) || !reader.read_point(target)) return;
					builder.cubic_to(origin + control_a, origin + control_b, origin + target);
					break;
				}
				case 'S':
				{
					point_t control_b{}, target{};
					if (!reader.read_point(control_b) || !reader.read_point(target)) return;
					builder.cubic_to(builder.reflected_cubic_control(), origin + control_b, origin + target);
					break;
				}
				case 'Q':
				{
					point_t control{}, target{};
					if (!reader.read_point(control) || !reader.read_point(target)) return;
					builder.quadratic_to(origin + control, origin + target);
					break;
				}
				case 'T':
				{
					point_t target{};
					if (!reader.read_point(target)) return;
					builder.quadratic_to(builder.reflected_quadratic_control(), origin + target);
					break;
				}
				case 'Z':
				{
					builder.close();
					builder.clear_smooth_state();
					break;
				}
				default:
				{
					// unsupported command (arcs), bail out rather than emitting garbage geometry
					builder.flush();
					return;
				}
				}
			}

			builder.flush();
		}

		/* --------------------------------------------------------------------- */
		/* primitives                                                             */
		/* --------------------------------------------------------------------- */

		void append_arc(contour_t& contour, point_t center, float radius_x, float radius_y, float start, float sweep, int segments)
		{
			for (int step = 0; step <= segments; ++step)
			{
				const float angle = start + sweep * (static_cast<float>(step) / static_cast<float>(segments));
				contour.points.push_back(point_t(center.x + std::cos(angle) * radius_x, center.y + std::sin(angle) * radius_y));
			}
		}

		void build_rounded_rect(std::vector<contour_t>& contours, float x, float y, float w, float h, float rx, float ry)
		{
			rx = std::clamp(rx, 0.f, w * 0.5f);
			ry = std::clamp(ry, 0.f, h * 0.5f);

			contour_t contour{};
			contour.closed = true;

			constexpr float half_pi = 1.57079632679489661923f;
			constexpr int corner_segments = 12;

			if (rx <= 0.f || ry <= 0.f)
			{
				contour.points = { point_t(x, y), point_t(x + w, y), point_t(x + w, y + h), point_t(x, y + h) };
				contours.push_back(std::move(contour));
				return;
			}

			append_arc(contour, point_t(x + w - rx, y + ry), rx, ry, -half_pi, half_pi, corner_segments);
			append_arc(contour, point_t(x + w - rx, y + h - ry), rx, ry, 0.f, half_pi, corner_segments);
			append_arc(contour, point_t(x + rx, y + h - ry), rx, ry, half_pi, half_pi, corner_segments);
			append_arc(contour, point_t(x + rx, y + ry), rx, ry, half_pi * 2.f, half_pi, corner_segments);

			contours.push_back(std::move(contour));
		}

		void build_ellipse(std::vector<contour_t>& contours, float cx, float cy, float rx, float ry)
		{
			contour_t contour{};
			contour.closed = true;

			constexpr int segments = 64;
			constexpr float two_pi = 6.28318530717958647692f;

			for (int step = 0; step < segments; ++step)
			{
				const float angle = two_pi * (static_cast<float>(step) / static_cast<float>(segments));
				contour.points.push_back(point_t(cx + std::cos(angle) * rx, cy + std::sin(angle) * ry));
			}

			contours.push_back(std::move(contour));
		}

		void parse_point_list(const std::string& data, contour_t& contour)
		{
			c_number_reader reader(data);

			point_t position{};
			while (reader.read_point(position))
				contour.points.push_back(position);
		}

		/* --------------------------------------------------------------------- */
		/* document                                                               */
		/* --------------------------------------------------------------------- */

		struct document_t
		{
			std::vector<element_t> elements{};
			bool has_view_box{ false };
			float view_box[4]{ 0.f, 0.f, 0.f, 0.f };
		};

		void resolve_paint(element_t& element, std::string_view attributes, std::string_view root_attributes)
		{
			const auto own_fill = find_attribute(attributes, "fill");
			const auto own_stroke = find_attribute(attributes, "stroke");

			element.fill = is_paint(own_fill);
			element.stroke = is_paint(own_stroke);

			if (!own_fill.has_value() && !own_stroke.has_value())
			{
				// nothing declared locally, inherit whatever the document root paints with.
				// an svg with no paint at all still fills, that is the specification default.
				const auto root_fill = find_attribute(root_attributes, "fill");
				const auto root_stroke = find_attribute(root_attributes, "stroke");

				element.fill = is_paint(root_fill) || (!root_fill.has_value() && !root_stroke.has_value());
				element.stroke = is_paint(root_stroke);
			}

			element.stroke_width = attribute_float(attributes, "stroke-width", 1.f);
			if (!own_stroke.has_value())
				element.stroke_width = attribute_float(root_attributes, "stroke-width", element.stroke_width);

			const auto fill_rule = find_attribute(attributes, "fill-rule");
			element.even_odd = fill_rule.has_value() && *fill_rule == "evenodd";
		}

		bool parse_document(const std::string& source, document_t& document)
		{
			std::string root_attributes{};

			std::size_t cursor = 0;
			while (cursor < source.size())
			{
				const std::size_t open = source.find('<', cursor);
				if (open == std::string::npos)
					break;

				std::size_t name_end = open + 1;
				while (name_end < source.size() && (std::isalnum(static_cast<unsigned char>(source[name_end])) || source[name_end] == '-'))
					++name_end;

				const std::string tag = source.substr(open + 1, name_end - open - 1);

				// walk to the closing bracket, quoted values may contain one
				std::size_t close = name_end;
				char quote = 0;
				while (close < source.size())
				{
					const char character = source[close];
					if (quote != 0)
					{
						if (character == quote) quote = 0;
					}
					else if (character == '"' || character == '\'')
					{
						quote = character;
					}
					else if (character == '>')
					{
						break;
					}
					++close;
				}

				if (close >= source.size())
					break;

				const std::string attributes = source.substr(name_end, close - name_end);
				cursor = close + 1;

				if (tag == "svg")
				{
					root_attributes = attributes;

					if (const auto view_box = find_attribute(attributes, "viewBox"); view_box.has_value())
					{
						c_number_reader reader(*view_box);
						if (reader.read(document.view_box[0]) && reader.read(document.view_box[1]) &&
							reader.read(document.view_box[2]) && reader.read(document.view_box[3]))
							document.has_view_box = document.view_box[2] > 0.f && document.view_box[3] > 0.f;
					}
					continue;
				}

				element_t element{};

				if (tag == "path")
				{
					const auto data = find_attribute(attributes, "d");
					if (!data.has_value() || data->empty())
						continue;

					parse_path_data(*data, element.contours);
				}
				else if (tag == "rect")
				{
					const float x = attribute_float(attributes, "x", 0.f);
					const float y = attribute_float(attributes, "y", 0.f);
					const float w = attribute_float(attributes, "width", 0.f);
					const float h = attribute_float(attributes, "height", 0.f);

					if (w <= 0.f || h <= 0.f)
						continue;

					const float rx = attribute_float(attributes, "rx", 0.f);
					const float ry = attribute_float(attributes, "ry", rx);

					build_rounded_rect(element.contours, x, y, w, h, rx, ry);
				}
				else if (tag == "circle")
				{
					const float radius = attribute_float(attributes, "r", 0.f);
					if (radius <= 0.f)
						continue;

					build_ellipse(element.contours, attribute_float(attributes, "cx", 0.f), attribute_float(attributes, "cy", 0.f), radius, radius);
				}
				else if (tag == "ellipse")
				{
					const float rx = attribute_float(attributes, "rx", 0.f);
					const float ry = attribute_float(attributes, "ry", 0.f);
					if (rx <= 0.f || ry <= 0.f)
						continue;

					build_ellipse(element.contours, attribute_float(attributes, "cx", 0.f), attribute_float(attributes, "cy", 0.f), rx, ry);
				}
				else if (tag == "line")
				{
					contour_t contour{};
					contour.points.push_back(point_t(attribute_float(attributes, "x1", 0.f), attribute_float(attributes, "y1", 0.f)));
					contour.points.push_back(point_t(attribute_float(attributes, "x2", 0.f), attribute_float(attributes, "y2", 0.f)));

					element.contours.push_back(std::move(contour));
				}
				else if (tag == "polygon" || tag == "polyline")
				{
					const auto points = find_attribute(attributes, "points");
					if (!points.has_value())
						continue;

					contour_t contour{};
					contour.closed = (tag == "polygon");
					parse_point_list(*points, contour);

					if (contour.points.size() < 2)
						continue;

					element.contours.push_back(std::move(contour));
				}
				else
				{
					continue;
				}

				if (element.contours.empty())
					continue;

				resolve_paint(element, attributes, root_attributes);

				if (!element.fill && !element.stroke)
					element.fill = true;

				// a line can never be filled, only stroked
				if (tag == "line" || tag == "polyline")
				{
					if (!element.stroke)
					{
						element.stroke = true;
						element.fill = false;
					}
				}

				document.elements.push_back(std::move(element));
			}

			return !document.elements.empty();
		}

		/* --------------------------------------------------------------------- */
		/* rasterization                                                          */
		/* --------------------------------------------------------------------- */

		void add_span(std::vector<float>& row, float x0, float x1, float weight)
		{
			const int width = static_cast<int>(row.size());

			x0 = std::max(x0, 0.f);
			x1 = std::min(x1, static_cast<float>(width));

			if (x1 <= x0)
				return;

			int first = static_cast<int>(std::floor(x0));
			int last = static_cast<int>(std::ceil(x1)) - 1;

			first = std::clamp(first, 0, width - 1);
			last = std::clamp(last, 0, width - 1);

			if (first == last)
			{
				row[first] += (x1 - x0) * weight;
				return;
			}

			row[first] += (static_cast<float>(first + 1) - x0) * weight;
			for (int index = first + 1; index < last; ++index)
				row[index] += weight;
			row[last] += (x1 - static_cast<float>(last)) * weight;
		}

		void build_edges(const std::vector<contour_t>& contours, std::vector<edge_t>& edges)
		{
			for (const auto& contour : contours)
			{
				const std::size_t count = contour.points.size();
				if (count < 2)
					continue;

				// filling always treats a contour as closed, exactly like a scanline filler should
				for (std::size_t index = 0; index < count; ++index)
				{
					const point_t& a = contour.points[index];
					const point_t& b = contour.points[(index + 1) % count];

					if (a.y == b.y)
						continue;

					if (a.y < b.y)
						edges.push_back(edge_t{ a.x, a.y, b.x, b.y, 1 });
					else
						edges.push_back(edge_t{ b.x, b.y, a.x, a.y, -1 });
				}
			}
		}

		struct crossing_t { float x{}; int direction{}; };

		void rasterize_contours(const std::vector<contour_t>& contours, bool even_odd, int width, int height, std::vector<float>& coverage)
		{
			std::vector<edge_t> edges{};
			build_edges(contours, edges);

			if (edges.empty())
				return;

			std::sort(edges.begin(), edges.end(), [](const edge_t& a, const edge_t& b) { return a.y0 < b.y0; });

			float min_y = edges.front().y0;
			float max_y = 0.f;
			for (const auto& edge : edges)
				max_y = std::max(max_y, edge.y1);

			const int first_row = std::clamp(static_cast<int>(std::floor(min_y)), 0, height - 1);
			const int last_row = std::clamp(static_cast<int>(std::ceil(max_y)), 0, height - 1);

			std::vector<float> row(static_cast<std::size_t>(width), 0.f);
			std::vector<const edge_t*> active{};
			std::vector<crossing_t> crossings{};

			std::size_t next_edge = 0;
			const float weight = 1.f / static_cast<float>(k_sub_samples);

			for (int pixel_y = first_row; pixel_y <= last_row; ++pixel_y)
			{
				const float row_top = static_cast<float>(pixel_y);
				const float row_bottom = row_top + 1.f;

				// pull in every edge that starts within or above this row
				while (next_edge < edges.size() && edges[next_edge].y0 < row_bottom)
					active.push_back(&edges[next_edge++]);

				// drop the ones that already ended
				active.erase(std::remove_if(active.begin(), active.end(),
					[row_top](const edge_t* edge) { return edge->y1 <= row_top; }), active.end());

				if (active.empty())
					continue;

				std::fill(row.begin(), row.end(), 0.f);
				bool touched = false;

				for (int sample = 0; sample < k_sub_samples; ++sample)
				{
					const float scan_y = row_top + (static_cast<float>(sample) + 0.5f) * weight;

					crossings.clear();
					for (const edge_t* edge : active)
					{
						if (scan_y < edge->y0 || scan_y >= edge->y1)
							continue;

						const float t = (scan_y - edge->y0) / (edge->y1 - edge->y0);
						crossings.push_back(crossing_t{ edge->x0 + (edge->x1 - edge->x0) * t, edge->direction });
					}

					if (crossings.size() < 2)
						continue;

					std::sort(crossings.begin(), crossings.end(), [](const crossing_t& a, const crossing_t& b) { return a.x < b.x; });

					if (even_odd)
					{
						for (std::size_t index = 0; index + 1 < crossings.size(); index += 2)
						{
							add_span(row, crossings[index].x, crossings[index + 1].x, weight);
							touched = true;
						}
					}
					else
					{
						int winding = 0;
						for (std::size_t index = 0; index + 1 < crossings.size(); ++index)
						{
							winding += crossings[index].direction;
							if (winding != 0)
							{
								add_span(row, crossings[index].x, crossings[index + 1].x, weight);
								touched = true;
							}
						}
					}
				}

				if (!touched)
					continue;

				float* destination = coverage.data() + static_cast<std::size_t>(pixel_y) * width;
				for (int pixel_x = 0; pixel_x < width; ++pixel_x)
					destination[pixel_x] = std::max(destination[pixel_x], std::min(1.f, row[pixel_x]));
			}
		}

		float signed_area(const std::vector<point_t>& points)
		{
			float area = 0.f;
			const std::size_t count = points.size();

			for (std::size_t index = 0; index < count; ++index)
			{
				const point_t& a = points[index];
				const point_t& b = points[(index + 1) % count];
				area += a.x * b.y - b.x * a.y;
			}

			return area * 0.5f;
		}

		void push_oriented(std::vector<contour_t>& target, std::vector<point_t> points)
		{
			if (points.size() < 3)
				return;

			// every stroke polygon has to wind the same way so the non zero rule unions them
			if (signed_area(points) > 0.f)
				std::reverse(points.begin(), points.end());

			contour_t contour{};
			contour.closed = true;
			contour.points = std::move(points);

			target.push_back(std::move(contour));
		}

		void build_stroke(const std::vector<contour_t>& contours, float width, std::vector<contour_t>& out)
		{
			const float half = std::max(width, 0.1f) * 0.5f;
			constexpr int joint_segments = 16;
			constexpr float two_pi = 6.28318530717958647692f;

			for (const auto& contour : contours)
			{
				const std::size_t count = contour.points.size();
				if (count == 0)
					continue;

				const std::size_t segments = contour.closed ? count : (count > 0 ? count - 1 : 0);

				for (std::size_t index = 0; index < segments; ++index)
				{
					const point_t& a = contour.points[index];
					const point_t& b = contour.points[(index + 1) % count];

					const point_t direction = b - a;
					const float length = direction.length();

					if (length <= 0.0001f)
						continue;

					const point_t normal(-direction.y / length * half, direction.x / length * half);

					push_oriented(out, { a + normal, b + normal, b - normal, a - normal });
				}

				// round joins and round caps, one disc per vertex
				for (const point_t& vertex : contour.points)
				{
					std::vector<point_t> disc{};
					disc.reserve(joint_segments);

					for (int step = 0; step < joint_segments; ++step)
					{
						const float angle = two_pi * (static_cast<float>(step) / static_cast<float>(joint_segments));
						disc.push_back(point_t(vertex.x + std::cos(angle) * half, vertex.y + std::sin(angle) * half));
					}

					push_oriented(out, std::move(disc));
				}
			}
		}
	}

	bool rasterize_source(const std::string& source, int canvas, bitmap_t& out, fit_mode mode, bool flip_horizontal)
	{
		if (canvas <= 0)
			return false;

		document_t document{};
		if (!parse_document(source, document))
		{
			slog::log::warning("svg: no drawable geometry found");
			return false;
		}

		/* bounds of the raw artwork, strokes excluded, matching the reference behaviour */
		float min_x = FLT_MAX, min_y = FLT_MAX, max_x = -FLT_MAX, max_y = -FLT_MAX;
		for (const auto& element : document.elements)
		{
			for (const auto& contour : element.contours)
			{
				for (const point_t& point : contour.points)
				{
					min_x = std::min(min_x, point.x);
					min_y = std::min(min_y, point.y);
					max_x = std::max(max_x, point.x);
					max_y = std::max(max_y, point.y);
				}
			}
		}

		if (min_x > max_x || min_y > max_y)
			return false;

		float scale = 1.f;
		float offset_x = 0.f;
		float offset_y = 0.f;

		if (mode == fit_mode::stretch && document.has_view_box)
		{
			scale = static_cast<float>(canvas) / std::max(document.view_box[2], document.view_box[3]);
			offset_x = -document.view_box[0] * scale;
			offset_y = -document.view_box[1] * scale;
		}
		else
		{
			const float bounds_width = std::max(max_x - min_x, 0.0001f);
			const float bounds_height = std::max(max_y - min_y, 0.0001f);
			const float largest = std::max(bounds_width, bounds_height);
			const float aspect = bounds_width / std::max(1.f, bounds_height);

			// optically boost wide or tall icons so every glyph reads at the same weight
			const float factor = (aspect > 1.35f || aspect < 0.74f) ? 0.95f : 0.90f;

			scale = (static_cast<float>(canvas) * factor) / largest;
			offset_x = static_cast<float>(canvas) * 0.5f - (min_x + bounds_width * 0.5f) * scale;
			offset_y = static_cast<float>(canvas) * 0.5f - (min_y + bounds_height * 0.5f) * scale;
		}

		for (auto& element : document.elements)
		{
			for (auto& contour : element.contours)
			{
				for (point_t& point : contour.points)
				{
					point.x = point.x * scale + offset_x;
					point.y = point.y * scale + offset_y;

					if (flip_horizontal)
						point.x = static_cast<float>(canvas) - point.x;
				}
			}
		}

		std::vector<float> coverage(static_cast<std::size_t>(canvas) * canvas, 0.f);

		for (const auto& element : document.elements)
		{
			if (element.fill)
				rasterize_contours(element.contours, element.even_odd, canvas, canvas, coverage);

			if (element.stroke)
			{
				std::vector<contour_t> outline{};
				build_stroke(element.contours, element.stroke_width * scale, outline);
				rasterize_contours(outline, false, canvas, canvas, coverage);
			}
		}

		out.width = canvas;
		out.height = canvas;
		out.pixels.assign(static_cast<std::size_t>(canvas) * canvas * 4u, 0u);

		for (std::size_t index = 0; index < coverage.size(); ++index)
		{
			const std::uint8_t alpha = static_cast<std::uint8_t>(std::clamp(coverage[index], 0.f, 1.f) * 255.f + 0.5f);

			out.pixels[index * 4 + 0] = 255;
			out.pixels[index * 4 + 1] = 255;
			out.pixels[index * 4 + 2] = 255;
			out.pixels[index * 4 + 3] = alpha;
		}

		return true;
	}

	bool rasterize_file(const std::string& path, int canvas, bitmap_t& out, fit_mode mode, bool flip_horizontal)
	{
		std::ifstream stream(path, std::ios::binary);
		if (!stream.is_open())
		{
			slog::log::error("svg: could not open {}", path);
			return false;
		}

		const std::string source((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
		return rasterize_source(source, canvas, out, mode, flip_horizontal);
	}
}
