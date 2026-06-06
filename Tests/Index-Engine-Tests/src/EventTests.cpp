#include <doctest/doctest.h>

#include "Utils/Event.hpp"

#include <stdexcept>
#include <string>
#include <vector>

using namespace Index;

TEST_CASE("A subscribed handler fires on Invoke with the right argument") {
	Event<int> event;

	int received = 0;
	int callCount = 0;
	event.Add([&](int value) {
		received = value;
		++callCount;
	});

	CHECK(event.HasListeners());

	event.Invoke(7);

	CHECK(callCount == 1);
	CHECK(received == 7);
}

TEST_CASE("Multiple handlers all fire on a single Invoke") {
	Event<int> event;

	int sum = 0;
	int hits = 0;
	event.Add([&](int value) { sum += value; ++hits; });
	event.Add([&](int value) { sum += value * 10; ++hits; });
	event.Add([&](int) { ++hits; });

	event.Invoke(3);

	CHECK(hits == 3);
	CHECK(sum == 3 + 30);
}

TEST_CASE("Invoke forwards multiple arguments to handlers") {
	Event<int, const std::string&> event;

	int gotNumber = 0;
	std::string gotText;
	event.Add([&](int n, const std::string& text) {
		gotNumber = n;
		gotText = text;
	});

	event.Invoke(42, std::string("hello"));

	CHECK(gotNumber == 42);
	CHECK(gotText == "hello");
}

TEST_CASE("A zero-argument event invokes its handler") {
	Event<> event;

	int calls = 0;
	event.Add([&]() { ++calls; });

	event.Invoke();
	event.Invoke();

	CHECK(calls == 2);
}

TEST_CASE("Remove with a valid id stops that handler from firing") {
	Event<int> event;

	int aCalls = 0;
	int bCalls = 0;
	EventId aId = event.Add([&](int) { ++aCalls; });
	event.Add([&](int) { ++bCalls; });

	CHECK(event.Remove(aId));

	event.Invoke(1);

	CHECK(aCalls == 0);
	CHECK(bCalls == 1);
}

TEST_CASE("Remove returns false for an id that is not registered") {
	Event<int> event;

	event.Add([&](int) {});

	const EventId bogus = EventId(999999);
	CHECK_FALSE(event.Remove(bogus));
}

TEST_CASE("Add hands out distinct ids") {
	Event<int> event;

	EventId first = event.Add([&](int) {});
	EventId second = event.Add([&](int) {});

	CHECK_FALSE(first == second);
}

TEST_CASE("Clear removes every listener") {
	Event<int> event;

	int calls = 0;
	event.Add([&](int) { ++calls; });
	event.Add([&](int) { ++calls; });

	CHECK(event.HasListeners());
	event.Clear();
	CHECK_FALSE(event.HasListeners());

	event.Invoke(1);

	CHECK(calls == 0);
}

TEST_CASE("HasListeners reflects the current subscription state") {
	Event<int> event;

	CHECK_FALSE(event.HasListeners());

	EventId id = event.Add([&](int) {});
	CHECK(event.HasListeners());

	event.Remove(id);
	CHECK_FALSE(event.HasListeners());
}

TEST_CASE("Handlers fire in subscription order") {
	Event<int> event;

	std::vector<int> order;
	event.Add([&](int) { order.push_back(1); });
	event.Add([&](int) { order.push_back(2); });
	event.Add([&](int) { order.push_back(3); });

	event.Invoke(0);

	REQUIRE(order.size() == 3);
	CHECK(order[0] == 1);
	CHECK(order[1] == 2);
	CHECK(order[2] == 3);
}

TEST_CASE("Calling Add from inside a callback throws to avoid self-deadlock") {
	Event<int> event;

	event.Add([&](int) {
		event.Add([](int) {});
	});

	CHECK_THROWS_AS(event.Invoke(1), std::logic_error);
}

TEST_CASE("Calling Clear from inside a callback throws to avoid self-deadlock") {
	Event<int> event;

	event.Add([&](int) {
		event.Clear();
	});

	CHECK_THROWS_AS(event.Invoke(1), std::logic_error);
}
