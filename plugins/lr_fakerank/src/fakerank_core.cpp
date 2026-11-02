#include "fakerank_core.hpp"

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

namespace lr_fakerank {
namespace {

char LowerCase(char c) {
	return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool EqualNoCase(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (std::size_t i = 0; i < a.size(); ++i) {
		if (LowerCase(a[i]) != LowerCase(b[i])) {
			return false;
		}
	}
	return true;
}

std::string_view Trim(std::string_view text) {
	std::size_t begin = 0;
	std::size_t end = text.size();
	while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
		++begin;
	}
	while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
		--end;
	}
	return text.substr(begin, end - begin);
}

// Valve's atoi semantics ("5 // x" -> 5, "1.5" -> 1, "abc" -> 0): std::from_chars first (it
// stops at the first non-digit exactly like atoi), std::atoi for a value it refuses.
int ParseIntText(std::string_view text) {
	text = Trim(text);
	if (text.empty()) {
		return 0;
	}
	int value = 0;
	const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
	if (result.ec != std::errc{}) {
		return std::atoi(std::string(text).c_str());
	}
	return value;
}

// A UTF-8 BOM would otherwise become part of the first key ("LR_FakeRank") and hide the whole
// table from the lookup below. The production file carries one.
constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

// The tokenizer and grammar of Valve's KeyValues (the subset the Pisex files use; the same parser
// the original loaded through hl2sdk):
//
//   file  := node* EOF
//   node  := token [ token ] [ '{' node* '}' ]
//   token := '"' escaped '"' | raw
class Parser {
public:
	explicit Parser(std::string_view text) : text_(text) {}

	bool Parse(KvNode& root) {
		ParseNodes(root, /*insideBlock=*/false);
		return !failed_;
	}

private:
	void SkipSpace() {
		while (pos_ < text_.size()) {
			const char c = text_[pos_];
			if (std::isspace(static_cast<unsigned char>(c))) {
				++pos_;
				continue;
			}
			if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '/') {
				pos_ += 2;
				while (pos_ < text_.size() && text_[pos_] != '\n') {
					++pos_;
				}
				continue;
			}
			if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '*') {
				pos_ += 2;
				bool closed = false;
				while (pos_ + 1 < text_.size()) {
					if (text_[pos_] == '*' && text_[pos_ + 1] == '/') {
						pos_ += 2;
						closed = true;
						break;
					}
					++pos_;
				}
				if (!closed) {
					// An unterminated block comment hides the rest of the file: malformed.
					pos_ = text_.size();
					failed_ = true;
					return;
				}
				continue;
			}
			break;
		}
	}

	// One token; false at the end of the input or at a '}'.
	bool ReadToken(std::string& out) {
		SkipSpace();
		if (failed_ || pos_ >= text_.size() || text_[pos_] == '}') {
			return false;
		}
		out.clear();
		if (text_[pos_] == '"') {
			++pos_;
			while (pos_ < text_.size()) {
				const char c = text_[pos_++];
				if (c == '\\' && pos_ < text_.size()) {
					const char esc = text_[pos_++];
					switch (esc) {
					case 'n':
						out.push_back('\n');
						break;
					case 'r':
						out.push_back('\r');
						break;
					case 't':
						out.push_back('\t');
						break;
					case '"':
						out.push_back('"');
						break;
					case '\\':
						out.push_back('\\');
						break;
					default:
						// Unknown escapes are kept verbatim, like the original's parser.
						out.push_back('\\');
						out.push_back(esc);
						break;
					}
					continue;
				}
				if (c == '"') {
					return true;
				}
				out.push_back(c);
			}
			failed_ = true; // unterminated string
			return true;    // keep what was read; the caller stops on failed_
		}
		while (pos_ < text_.size()) {
			const char c = text_[pos_];
			if (std::isspace(static_cast<unsigned char>(c)) || c == '{' || c == '}' || c == '"') {
				break;
			}
			// A comment starts a new token boundary: "value/*c*/" reads as "value".
			if (c == '/' && pos_ + 1 < text_.size() && (text_[pos_ + 1] == '/' || text_[pos_ + 1] == '*')) {
				break;
			}
			out.push_back(c);
			++pos_;
		}
		return true;
	}

	bool PeekIs(char c) {
		SkipSpace();
		return !failed_ && pos_ < text_.size() && text_[pos_] == c;
	}

	void ParseNodes(KvNode& parent, bool insideBlock) {
		while (!failed_) {
			SkipSpace();
			if (failed_) {
				return;
			}
			if (pos_ >= text_.size()) {
				if (insideBlock) {
					failed_ = true; // missing '}'
				}
				return;
			}
			if (text_[pos_] == '}') {
				if (!insideBlock) {
					failed_ = true; // stray '}'
				}
				++pos_;
				return;
			}
			std::string key;
			if (!ReadToken(key)) {
				return;
			}
			if (failed_) {
				return;
			}
			if (!key.empty() && key[0] == '#') {
				// #base / #include / #define: no inclusion, skip the line.
				while (pos_ < text_.size() && text_[pos_] != '\n') {
					++pos_;
				}
				continue;
			}
			parent.children.push_back(KvNode{});
			// Filled through an index: the recursion below may reallocate parent.children.
			const std::size_t index = parent.children.size() - 1;
			parent.children[index].name = std::move(key);
			if (!PeekIs('{')) {
				if (failed_) {
					return;
				}
				std::string value;
				if (ReadToken(value)) {
					if (failed_) {
						return;
					}
					parent.children[index].value = std::move(value);
					parent.children[index].hasValue = true;
				}
			}
			if (PeekIs('{')) {
				if (failed_) {
					return;
				}
				++pos_;
				KvNode child = std::move(parent.children[index]);
				ParseNodes(child, /*insideBlock=*/true);
				parent.children[index] = std::move(child);
				if (failed_) {
					return;
				}
			}
		}
	}

	std::string_view text_;
	std::size_t pos_ = 0;
	bool failed_ = false;
};

} // namespace

// --- the KV1 reader -------------------------------------------------------------------------------

const KvNode* KvNode::Find(std::string_view key) const {
	if (key.empty()) {
		return nullptr;
	}
	for (const KvNode& child : children) {
		if (EqualNoCase(child.name, key)) {
			return &child;
		}
	}
	return nullptr;
}

std::string_view KvNode::GetString(std::string_view key, std::string_view def) const {
	const KvNode* found = Find(key);
	if (found == nullptr || !found->hasValue) {
		return def;
	}
	return found->value;
}

int KvNode::GetInt(std::string_view key, int def) const {
	const KvNode* found = Find(key);
	if (found == nullptr || !found->hasValue) {
		return def;
	}
	const std::string_view text = Trim(found->value);
	if (text.empty()) {
		return def;
	}
	return ParseIntText(text);
}

int KvNode::IntValue() const {
	if (!hasValue) {
		return 0;
	}
	return ParseIntText(value);
}

std::optional<KvNode> ParseKv1(std::string_view text) {
	if (text.starts_with(kUtf8Bom)) {
		text.remove_prefix(kUtf8Bom.size());
	}
	KvNode root;
	Parser parser(text);
	if (!parser.Parse(root)) {
		return std::nullopt;
	}
	// A single top-level block is the file's root ("LR_FakeRank" { ... }), as in Valve's loader.
	if (root.children.size() == 1 && !root.children.front().children.empty()) {
		return std::move(root.children.front());
	}
	return root;
}

std::optional<KvNode> LoadKv1File(const std::string& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return std::nullopt;
	}
	std::ostringstream content;
	content << file.rdbuf();
	return ParseKv1(content.str());
}

// --- the configuration location -------------------------------------------------------------------

std::string JoinPath(std::string_view base, std::string_view name) {
	if (base.empty()) {
		return std::string(name);
	}
	if (name.empty()) {
		return std::string(base);
	}
	std::string out(base);
	if (out.back() != '/') {
		out.push_back('/');
	}
	if (name.front() == '/') {
		name.remove_prefix(1);
	}
	out.append(name);
	return out;
}

bool FileExists(const std::string& path) {
	if (path.empty()) {
		return false;
	}
	std::ifstream file(path, std::ios::binary);
	return file.good();
}

std::vector<std::string> ConfigCandidates(std::string_view configsDir) {
	std::vector<std::string> candidates;
	if (configsDir.empty()) {
		return candidates;
	}
	candidates.reserve(2);
	candidates.push_back(JoinPath(JoinPath(configsDir, kLevelsRanksDir), kConfigFile));
	candidates.push_back(JoinPath(JoinPath(configsDir, kPluginDir), kConfigFile));
	return candidates;
}

ConfigSource LoadConfig(std::string_view configsDir, Config& out) {
	ConfigSource source;
	const std::vector<std::string> candidates = ConfigCandidates(configsDir);
	if (candidates.empty()) {
		source.error = "the Plugify configs directory is unavailable";
		return source;
	}
	for (const std::string& path : candidates) {
		if (!FileExists(path)) {
			continue;
		}
		source.path = path;
		const std::optional<KvNode> root = LoadKv1File(path);
		if (!root) {
			source.error = "failed to load " + path + " (not valid KeyValues)";
			return source;
		}
		ParseConfig(*root, out, &source.problem);
		source.loaded = true;
		return source;
	}
	std::string tried;
	for (const std::string& path : candidates) {
		if (!tried.empty()) {
			tried += " or ";
		}
		tried += path;
	}
	source.error = "failed to load " + tried;
	return source;
}

// --- the configuration model ----------------------------------------------------------------------

int Config::RankFor(int rank) const {
	const auto found = ranks.find(rank);
	return found == ranks.end() ? 0 : found->second; // std::map::operator[] of the original
}

void ParseConfig(const KvNode& root, Config& out, std::string* problem) {
	// LoadConfig() of the original: the rank table is cleared first, then the Type switch (the
	// derived type of the four known values; anything else leaves the previous type alone).
	out.ranks.clear();
	switch (root.GetInt("Type", 0)) {
	case 0:
		out.rank_type = 12;
		break;
	case 1:
		out.rank_type = 7;
		break;
	case 2:
		out.rank_type = 10;
		break;
	case 3:
		out.rank_type = 11;
		break;
	case 4:
		out.rank_type = 13;
		break;
	default:
		break;
	}

	const KvNode* fake = root.Find("FakeRank");
	if (fake == nullptr) {
		return; // the original's missing FakeRank key left its FOR_EACH_VALUE loop unentered
	}
	for (const KvNode& value : fake->children) {
		// FOR_EACH_VALUE walks the value nodes only: a nested block has no value and is skipped.
		if (!value.hasValue || value.name.empty()) {
			continue;
		}
		const std::string_view text(value.name);
		int rank = 0;
		const auto result = std::from_chars(text.data(), text.data() + text.size(), rank);
		if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
			// The original's std::stoi threw here (a config typo took the server down with it).
			if (problem != nullptr) {
				*problem = "the FakeRank key \"" + std::string(text) + "\" is not a rank number";
			}
			continue;
		}
		out.ranks[rank] = value.IntValue();
	}
}

Desired ComputeDesired(const Config& config, int lr_rank, int lr_exp) {
	Desired desired;
	desired.wins = 777;
	switch (config.rank_type) {
	case 11:
		// Type 3 (Premier): the raw experience is the rating, the rank type 11.
		desired.ranking = lr_exp;
		desired.rank_type = 11;
		break;
	case 13:
		// Type 4 (Premier with a mapped rank): the tab rank of the LR rank, the rank type 11.
		desired.ranking = config.RankFor(lr_rank);
		desired.rank_type = 11;
		break;
	default:
		desired.ranking = config.RankFor(lr_rank);
		desired.rank_type = config.rank_type;
		break;
	}
	return desired;
}

// --- the map-gated schema offset cache -------------------------------------------------------------

int32_t SchemaOffsetCache::Offset(bool mapReady, const char* className, const char* fieldName,
								  OffsetLookup lookup) {
	if (className == nullptr || fieldName == nullptr || *className == '\0' || *fieldName == '\0' ||
		lookup == nullptr) {
		return -1;
	}
	std::string key(className);
	key.push_back('\0');
	key.append(fieldName);
	if (const auto cached = m_offsets.find(key); cached != m_offsets.end()) {
		return cached->second;
	}
	if (!mapReady) {
		return -1; // GetSchemaOffset would dereference the null serializer map
	}
	const int32_t offset = lookup(className, fieldName);
	if (offset >= 0) {
		m_offsets.emplace(std::move(key), offset);
	}
	return offset;
}

// --- the reveal state machine ----------------------------------------------------------------------

bool ShouldReveal(RevealState& state, int slot, uint64_t buttons, int reveal_mode) {
	if (slot < 0 || slot >= kMaxSlots) {
		return false;
	}
	const std::size_t index = static_cast<std::size_t>(slot);
	const bool pressed_tab =
		(buttons & kButtonScoreboard) != 0 && (state.old_buttons[index] & kButtonScoreboard) == 0;
	state.old_buttons[index] = buttons;
	if (!pressed_tab) {
		return false;
	}
	const bool wanted = reveal_mode == kRevealEveryPress ||
						(reveal_mode == kRevealOncePerMap && !state.revealed[index]);
	if (!wanted) {
		return false;
	}
	state.revealed[index] = true;
	return true;
}

void MarkDisconnected(RevealState& state, int slot) {
	if (slot >= 0 && slot < kMaxSlots) {
		state.revealed[static_cast<std::size_t>(slot)] = false;
	}
}

void ResetRevealed(RevealState& state) {
	state.revealed.fill(false);
}

} // namespace lr_fakerank
