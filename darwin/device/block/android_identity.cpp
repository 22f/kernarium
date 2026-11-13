// SPDX-License-Identifier: GPL-2.0-or-later

#include "android_identity.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace uml::android {
namespace {

constexpr std::string_view hexDigits{"0123456789abcdef"};
constexpr unsigned int identitySchema = 1;
/* No identity file is larger than this; a file that is, is not one. */
constexpr std::size_t maximumFileSize = 4096;
constexpr unsigned int generationAttempts = 64;

[[noreturn]] void failWith(int error, const std::string &what)
{
	throw std::system_error(error, std::generic_category(), what);
}

[[noreturn]] void invalid(const std::filesystem::path &file,
			  const char *reason)
{
	throw std::invalid_argument("the identity file " + file.string() +
				    " is not valid: " + reason);
}

int hexValue(char digit) noexcept
{
	if (digit >= '0' && digit <= '9')
		return digit - '0';
	if (digit >= 'a' && digit <= 'f')
		return digit - 'a' + 10;
	if (digit >= 'A' && digit <= 'F')
		return digit - 'A' + 10;
	return -1;
}

/* One member of the file's object: a string or a non-negative integer. */
struct Member {
	std::string key;
	bool isNumber = false;
	std::string text;
	unsigned long number = 0;
};

/* The JSON the identity file uses: one object of strings and non-negative
 * integers. Strings carry no escape and no control character. Anything else is
 * not an identity file. */
class FlatObjectParser final {
public:
	FlatObjectParser(std::string_view text, const std::filesystem::path &file)
		: text_(text), file_(file)
	{
	}

	std::vector<Member> parse()
	{
		std::vector<Member> members;
		skipSpace();
		expect('{');
		skipSpace();
		if (peek() == '}') {
			++cursor_;
		} else {
			for (;;) {
				Member member;
				skipSpace();
				member.key = parseString();
				skipSpace();
				expect(':');
				skipSpace();
				if (peek() == '"') {
					member.text = parseString();
				} else {
					member.isNumber = true;
					member.number = parseNumber();
				}
				members.push_back(std::move(member));
				skipSpace();
				if (peek() == ',') {
					++cursor_;
					continue;
				}
				expect('}');
				break;
			}
		}
		skipSpace();
		if (cursor_ != text_.size())
			invalid(file_, "it has content after the object");
		return members;
	}

private:
	char peek() const noexcept
	{
		return cursor_ < text_.size() ? text_[cursor_] : '\0';
	}

	void skipSpace() noexcept
	{
		while (cursor_ < text_.size() &&
		       (text_[cursor_] == ' ' || text_[cursor_] == '\n' ||
			text_[cursor_] == '\t' || text_[cursor_] == '\r'))
			++cursor_;
	}

	void expect(char token)
	{
		if (peek() != token)
			invalid(file_, "it is not the expected JSON object");
		++cursor_;
	}

	std::string parseString()
	{
		expect('"');
		std::string value;
		while (cursor_ < text_.size() && text_[cursor_] != '"') {
			const unsigned char character =
				static_cast<unsigned char>(text_[cursor_++]);
			if (character < 0x20 || character == '\\' ||
			    character >= 0x7f)
				invalid(file_, "a string holds an escape or a "
					       "character it never does");
			value.push_back(static_cast<char>(character));
		}
		expect('"');
		return value;
	}

	unsigned long parseNumber()
	{
		const auto begin = cursor_;
		while (cursor_ < text_.size() && text_[cursor_] >= '0' &&
		       text_[cursor_] <= '9')
			++cursor_;
		const auto digits = text_.substr(begin, cursor_ - begin);
		if (digits.empty() || digits.size() > 9 ||
		    (digits.size() > 1 && digits.front() == '0'))
			invalid(file_, "a number is not a plain integer");
		return std::strtoul(std::string{digits}.c_str(), nullptr, 10);
	}

	std::string_view text_;
	const std::filesystem::path &file_;
	std::size_t cursor_ = 0;
};

class Descriptor final {
public:
	Descriptor(const std::filesystem::path &path, int flags, mode_t mode = 0)
		: descriptor_(open(path.c_str(), flags | O_CLOEXEC, mode))
	{
		if (descriptor_ < 0)
			failWith(errno, "cannot open " + path.string());
	}
	~Descriptor()
	{
		if (descriptor_ >= 0)
			(void)close(descriptor_);
	}
	Descriptor(const Descriptor &) = delete;
	Descriptor &operator=(const Descriptor &) = delete;

	int get() const noexcept { return descriptor_; }

	void closeChecked(const std::string &what)
	{
		const int result = close(descriptor_);
		descriptor_ = -1;
		if (result != 0)
			failWith(errno, what);
	}

private:
	int descriptor_;
};

std::string serialFrom(std::span<const std::uint8_t> bytes)
{
	std::string serial;
	serial.reserve(bytes.size() * 2U);
	for (const auto byte : bytes) {
		serial.push_back(hexDigits[byte >> 4]);
		serial.push_back(hexDigits[byte & 0x0fU]);
	}
	return serial;
}

std::string render(const InstallationIdentity &identity)
{
	return "{\n  \"ethernet_mac\": \"" + formatMac(*identity.ethernetMac) +
	       "\",\n  \"schema\": " + std::to_string(identitySchema) +
	       ",\n  \"serialno\": \"" + *identity.serial + "\"\n}\n";
}

} // namespace

std::filesystem::path identityFile(const std::filesystem::path &dataDisk)
{
	return dataDisk.parent_path() / identityFileName;
}

bool validSerial(std::string_view serial) noexcept
{
	if (serial.size() < 6U || serial.size() > 20U)
		return false;
	for (const char character : serial) {
		const bool digit = character >= '0' && character <= '9';
		const bool letter = (character >= 'a' && character <= 'z') ||
				    (character >= 'A' && character <= 'Z');
		if (!digit && !letter)
			return false;
	}
	constexpr std::string_view none{"unknown"};
	return !std::ranges::equal(serial, none, [](char left, char right) {
		return (left >= 'A' && left <= 'Z' ? left - 'A' + 'a' : left) ==
		       right;
	});
}

bool validEthernetMac(const MacAddress &mac) noexcept
{
	constexpr MacAddress none{0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
	return (mac[0] & 0x03U) == 0x02U && mac != none &&
	       mac != defaultEthernetMac && mac != legacyWifiMac;
}

std::string formatMac(const MacAddress &mac)
{
	std::string text;
	text.reserve(17);
	for (std::size_t index = 0; index < mac.size(); ++index) {
		if (index != 0)
			text.push_back(':');
		text.push_back(hexDigits[mac[index] >> 4]);
		text.push_back(hexDigits[mac[index] & 0x0fU]);
	}
	return text;
}

std::optional<MacAddress> parseMac(std::string_view text) noexcept
{
	if (text.size() != 17U)
		return std::nullopt;
	MacAddress mac{};
	for (std::size_t index = 0; index < mac.size(); ++index) {
		const auto offset = index * 3U;
		if (index != 0 && text[offset - 1U] != ':')
			return std::nullopt;
		const int high = hexValue(text[offset]);
		const int low = hexValue(text[offset + 1U]);
		if (high < 0 || low < 0)
			return std::nullopt;
		mac[index] = static_cast<std::uint8_t>(high << 4 | low);
	}
	return mac;
}

void systemRandom(std::span<std::uint8_t> bytes)
{
	arc4random_buf(bytes.data(), bytes.size());
}

InstallationIdentity generateIdentity(const RandomFill &random)
{
	for (unsigned int attempt = 0; attempt < generationAttempts; ++attempt) {
		std::array<std::uint8_t, 8> serial{};
		MacAddress mac{};
		random(serial);
		random(mac);
		/* Locally administered, unicast: the other 46 bits are random. */
		mac[0] = static_cast<std::uint8_t>((mac[0] & 0xfcU) | 0x02U);
		if (validEthernetMac(mac))
			return {serialFrom(serial), mac};
	}
	throw std::runtime_error(
		"the random source gave no usable installation identity");
}

std::optional<InstallationIdentity> readIdentityFile(
	const std::filesystem::path &file)
{
	struct stat details {};
	if (lstat(file.c_str(), &details) != 0) {
		if (errno == ENOENT)
			return std::nullopt;
		failWith(errno, "cannot examine " + file.string());
	}
	if (!S_ISREG(details.st_mode))
		invalid(file, "it is not a regular file");
	if (details.st_size < 0 ||
	    static_cast<std::uint64_t>(details.st_size) > maximumFileSize)
		invalid(file, "it is too large");
	std::string text(static_cast<std::size_t>(details.st_size), '\0');
	std::FILE *handle = std::fopen(file.c_str(), "rb");
	if (handle == nullptr)
		failWith(errno, "cannot open " + file.string());
	const auto count = std::fread(text.data(), 1, text.size(), handle);
	const int closed = std::fclose(handle);
	if (count != text.size() || closed != 0)
		throw std::runtime_error("cannot read " + file.string());

	const auto members = FlatObjectParser{text, file}.parse();
	if (members.size() != 3U)
		invalid(file, "it has other members than schema, serialno and "
			      "ethernet_mac");
	const Member *schema = nullptr;
	const Member *serial = nullptr;
	const Member *mac = nullptr;
	for (const auto &member : members) {
		const Member **slot = member.key == "schema" ? &schema :
				      member.key == "serialno" ? &serial :
				      member.key == "ethernet_mac" ? &mac :
								     nullptr;
		if (slot == nullptr || *slot != nullptr)
			invalid(file, "it has an unknown or repeated member");
		*slot = &member;
	}
	if (schema == nullptr || !schema->isNumber ||
	    schema->number != identitySchema)
		invalid(file, "its schema is not 1");
	if (serial == nullptr || serial->isNumber || !validSerial(serial->text))
		invalid(file, "its serial number is not valid");
	std::optional<MacAddress> address;
	if (mac != nullptr && !mac->isNumber)
		address = parseMac(mac->text);
	if (!address || !validEthernetMac(*address))
		invalid(file, "its ethernet address is not valid");
	return InstallationIdentity{serial->text, *address};
}

void writeIdentityFile(const std::filesystem::path &file,
		       const InstallationIdentity &identity)
{
	if (!identity.serial || !validSerial(*identity.serial) ||
	    !identity.ethernetMac || !validEthernetMac(*identity.ethernetMac))
		throw std::invalid_argument(
			"an identity file holds a valid serial number and "
			"ethernet address");
	const auto content = render(identity);
	const auto temporary = file.parent_path() /
		(file.filename().string() + ".tmp-" + std::to_string(getpid()));
	(void)unlink(temporary.c_str());
	try {
		Descriptor descriptor{temporary, O_WRONLY | O_CREAT | O_EXCL,
				      0600};
		std::size_t done = 0;
		while (done < content.size()) {
			const auto count = write(descriptor.get(),
						 content.data() + done,
						 content.size() - done);
			if (count < 0 && errno == EINTR)
				continue;
			if (count <= 0)
				failWith(count < 0 ? errno : EIO,
					 "cannot write " + temporary.string());
			done += static_cast<std::size_t>(count);
		}
		if (fsync(descriptor.get()) != 0)
			failWith(errno, "cannot flush " + temporary.string());
		descriptor.closeChecked("cannot close " + temporary.string());
		if (rename(temporary.c_str(), file.c_str()) != 0)
			failWith(errno, "cannot replace " + file.string());
	} catch (...) {
		(void)unlink(temporary.c_str());
		throw;
	}
}

ResolvedIdentity resolveIdentity(const IdentityRequest &request)
{
	if (request.stated.serial || request.stated.ethernetMac)
		return {request.stated, IdentityOutcome::stated};
	if (request.file.empty())
		return {};

	auto outcome = IdentityOutcome::created;
	if (!request.diskCreated) {
		try {
			if (auto kept = readIdentityFile(request.file))
				return {*kept, IdentityOutcome::kept};
		} catch (const std::invalid_argument &) {
			outcome = IdentityOutcome::replaced;
		} catch (const std::system_error &) {
			/* A file that exists and cannot be read is not replaced:
			 * this start takes new values and the file keeps its. */
			return {generateIdentity(request.random),
				IdentityOutcome::unsaved};
		}
	}
	auto fresh = generateIdentity(request.random);
	try {
		writeIdentityFile(request.file, fresh);
	} catch (const std::system_error &) {
		outcome = IdentityOutcome::unsaved;
	}
	return {std::move(fresh), outcome};
}

std::string serialArgument(std::string_view serial)
{
	return std::string{serialPrefix} + std::string{serial};
}

} // namespace uml::android
