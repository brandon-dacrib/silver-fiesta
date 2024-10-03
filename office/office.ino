// office.ino

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Inkplate.h>
#include <TimeLib.h>
#include <WiFiUdp.h>
#include <esp_sleep.h>
// Removed <time.h> to prevent conflicts with TimeLib.h
#include "config.h"  // Include your configuration header

// Include fonts using relative paths
#include "FreeMonoBold24pt7b.h"       // Font for status messages
#include "FreeMonoBoldOblique12pt7b.h" // Font for event titles

Inkplate display;
WiFiUDP ntpUDP;
const char* ntpServer = "pool.ntp.org";

// Time zone offset for Eastern Time (New York)
// Eastern Standard Time (EST) is UTC-5 hours
// Eastern Daylight Time (EDT) is UTC-4 hours
// We'll need to handle daylight saving time

// First, define EventDetails struct
struct EventDetails {
    String calendarName;
    String message;
    time_t startTime;
    time_t endTime;
    bool highlightRed = false;
};

String lastRefreshTime;  // Store last refresh time

// Function to format time to HH:MM AM/PM format
String formatTime(time_t dateTime) {
    char formattedTime[16];
    int hr = hour(dateTime);
    snprintf(formattedTime, sizeof(formattedTime), "%02d:%02d %s", (hr % 12 == 0) ? 12 : hr % 12, minute(dateTime), (hr >= 12) ? "PM" : "AM");
    return String(formattedTime);
}

// Function to format date to MM/DD/YYYY format
String formatDate(time_t dateTime) {
    char formattedDate[16];
    snprintf(formattedDate, sizeof(formattedDate), "%02d/%02d/%04d", month(dateTime), day(dateTime), year(dateTime));
    return String(formattedDate);
}

// Function to format date and time for debugging
String formatDateTime(time_t dateTime) {
    char buffer[25];
    snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d",
             year(dateTime), month(dateTime), day(dateTime),
             hour(dateTime), minute(dateTime), second(dateTime));
    return String(buffer);
}

// Function to parse a date-time string into a time_t
time_t parseDateTime(const char* dateTimeStr) {
    int yy, MM, dd, hh, mm, ss;
    if (sscanf(dateTimeStr, "%d-%d-%d %d:%d:%d", &yy, &MM, &dd, &hh, &mm, &ss) == 6) {
        tmElements_t tm;
        tm.Year = CalendarYrToTm(yy);
        tm.Month = MM;
        tm.Day = dd;
        tm.Hour = hh;
        tm.Minute = mm;
        tm.Second = ss;
        return makeTime(tm);
    } else {
        Serial.println("Failed to parse date-time string");
        return 0;
    }
}

// Function to map calendar entity_id to human-readable name
String getCalendarName(const char* homeAssistantURL) {
    for (int i = 0; i < sizeof(calendarMap) / sizeof(calendarMap[0]); i++) {
        if (String(homeAssistantURL) == calendarMap[i].url) {
            return String(calendarMap[i].name);
        }
    }
    return "Unknown Calendar";
}

// Function to truncate a long event title
String truncateTitle(String title) {
    if (title.length() > 30) {  // Assuming 30 characters fit on one line
        return title.substring(0, 27) + "...";
    }
    return title;
}

// Function to reconnect WiFi on wake-up
bool reconnectWiFi() {
    WiFi.disconnect();
    WiFi.begin(ssid, password);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 10) {
        delay(1000);
        attempts++;
    }
    return WiFi.status() == WL_CONNECTED;
}

// Function to display error messages with large font in the upper left-hand corner
void displayError(String errorMessage) {
    display.clearDisplay();
    display.setFont(&FreeMonoBold24pt7b);  // Use large bold font
    display.setTextSize(1);  // Text size 1 with large font
    display.setTextColor(INKPLATE_RED);
    display.setCursor(10, 50);  // Upper left-hand corner
    display.print(errorMessage);
    display.display();
}

// Function to determine if daylight saving time is in effect
bool isDST(int dayOfMonth, int month, int dayOfWeek) {
    // DST starts on the second Sunday in March
    // DST ends on the first Sunday in November

    if (month < 3 || month > 11) return false;  // Not DST in Jan, Feb, Dec

    if (month > 3 && month < 11) return true;   // DST in Apr - Oct

    int previousSunday = dayOfMonth - dayOfWeek;

    if (month == 3) {  // March
        // DST starts at 2 am on the second Sunday in March
        return previousSunday >= 8;
    } else if (month == 11) {  // November
        // DST ends at 2 am on the first Sunday in November
        return previousSunday < 1;
    }

    return false;
}

int getGMTOffset() {
    // Calculate the GMT offset based on whether DST is in effect
    time_t nowTime = now();
    int dayOfWeek = weekday(nowTime) - 1;  // TimeLib weekday() returns 1-7 for Sun-Sat
    int dayOfMonth = day(nowTime);
    int monthOfYear = month(nowTime);

    if (isDST(dayOfMonth, monthOfYear, dayOfWeek)) {
        return -4 * 3600;  // EDT is UTC-4
    } else {
        return -5 * 3600;  // EST is UTC-5
    }
}

// Function to set time from NTP server
void setRTCFromNTP() {
    configTime(0, 0, ntpServer);  // Temporarily set GMT offset and daylight offset to 0
    time_t nowTime = time(nullptr);
    int attempts = 0;
    while (nowTime < 8 * 3600 * 2 && attempts < 10) {  // Wait for time to be set
        delay(500);
        nowTime = time(nullptr);
        attempts++;
    }
    if (attempts >= 10) {
        Serial.println("Failed to obtain time from NTP");
    } else {
        // Adjust time based on GMT offset and daylight saving time
        int gmtOffset = getGMTOffset();
        nowTime += gmtOffset;
        setTime(nowTime);  // Set time for TimeLib functions
        Serial.println("RTC updated from NTP");
    }
}

// Function to fetch event details
EventDetails fetchEventDetails(const char* homeAssistantURL) {
    HTTPClient http;
    http.setTimeout(15000);  // Set a timeout of 15 seconds
    http.begin(homeAssistantURL);  
    http.addHeader("Authorization", homeAssistantToken);  

    Serial.print("Making HTTP GET request to: ");
    Serial.println(homeAssistantURL);

    int httpCode = http.GET();

    EventDetails details;

    if (httpCode == 200) {
        Serial.println("HTTP GET successful");
        String payload = http.getString();
        DynamicJsonDocument doc(2048);  // Adjust size based on response
        DeserializationError error = deserializeJson(doc, payload);

        if (!error) {
            const char* startTimeStr = doc["attributes"]["start_time"].as<const char*>();
            const char* endTimeStr = doc["attributes"]["end_time"].as<const char*>();

            Serial.print("Start Time String: ");
            Serial.println(startTimeStr);
            Serial.print("End Time String: ");
            Serial.println(endTimeStr);

            details.message = doc["attributes"]["message"].as<const char*>();
            if (details.message == "") {
                details.message = "Busy";
            }

            details.startTime = parseDateTime(startTimeStr);
            details.endTime = parseDateTime(endTimeStr);

            // Adjust event times for time zone
            int gmtOffset = getGMTOffset();
            details.startTime += gmtOffset;
            details.endTime += gmtOffset;

            // Verify parsed times
            Serial.print("Parsed Start Time (timestamp): ");
            Serial.println(details.startTime);
            Serial.print("Parsed End Time (timestamp): ");
            Serial.println(details.endTime);

            Serial.print("Parsed Start Time (formatted): ");
            Serial.println(formatDateTime(details.startTime));
            Serial.print("Parsed End Time (formatted): ");
            Serial.println(formatDateTime(details.endTime));

        } else {
            displayError("JSON Parse Error");
            Serial.print("JSON Parse Error: ");
            Serial.println(error.c_str());
        }
    } else {
        String errorString = http.errorToString(httpCode);
        Serial.print("HTTP GET failed, error: ");
        Serial.println(errorString);
        displayError("API Error: " + errorString);
    }

    http.end();
    return details;
}

// Function to check if an event is today
bool isEventToday(time_t eventTime) {
    return (day(eventTime) == day()) &&
           (month(eventTime) == month()) &&
           (year(eventTime) == year());
}

// Function to check if an event is during the refresh period
bool isEventDuringRefresh(EventDetails event) {
    time_t nowTime = now();  // Current time
    time_t refreshPeriodEnd = nowTime + (15 * 60);  // 15 minutes ahead
    return (event.startTime <= refreshPeriodEnd && event.endTime >= nowTime);
}

// Function to sort events by start time
void sortEventsByStartTime(EventDetails arr[], int n) {
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - i - 1; j++) {
            if (arr[j].startTime > arr[j + 1].startTime) {
                EventDetails temp = arr[j];
                arr[j] = arr[j + 1];
                arr[j + 1] = temp;
            }
        }
    }
}

// Function to display calendar events
void displayCalendarEvents() {
    display.clearDisplay();

    time_t currentTime = now();  // Current time from RTC
    int cursorY = 70;

    // Print current date in the upper left corner
    display.setFont(&FreeMonoBold24pt7b);
    display.setTextColor(INKPLATE_BLACK);
    display.setTextSize(1);
    display.setCursor(10, 30);
    display.print(formatDate(currentTime));

    const int MAX_EVENTS = 10;
    EventDetails events[MAX_EVENTS];
    int eventCount = 0;
    int ongoingEventsCount = 0;

    int numberOfURLs = sizeof(homeAssistantURLs) / sizeof(homeAssistantURLs[0]);

    for (int i = 0; i < numberOfURLs && eventCount < MAX_EVENTS; i++) {
        EventDetails eventDetails = fetchEventDetails(homeAssistantURLs[i]);
        eventDetails.calendarName = getCalendarName(homeAssistantURLs[i]);

        if (eventDetails.startTime == 0 || eventDetails.endTime == 0) {
            Serial.println("Invalid start or end time, skipping event");
            continue;  // Skip events with invalid times
        }

        events[eventCount++] = eventDetails;

        // Check if the event is during the refresh period
        if (isEventDuringRefresh(eventDetails)) {
            ongoingEventsCount++;
        }
    }

    // Sort events by start time
    sortEventsByStartTime(events, eventCount);

    int numEventsToShow = min(eventCount, 5);

    bool lineDrawn = false;

    for (int i = 0; i < numEventsToShow; ++i) {
        EventDetails& event = events[i];

        // Determine if the event is today
        bool eventIsToday = isEventToday(event.startTime);

        // Draw a large thick yellow horizontal line between today's events and future events
        if (!eventIsToday && !lineDrawn) {
            int lineThickness = 10;  // Adjust the thickness as needed
            display.fillRect(0, cursorY - 20, display.width(), lineThickness, INKPLATE_YELLOW);
            lineDrawn = true;
            cursorY += 10;  // Adjust spacing after the line
        }

        if (eventIsToday) {
            if (i == 0) {
                display.setTextColor(INKPLATE_GREEN);  // Next event today
            } else {
                display.setTextColor(INKPLATE_BLUE);   // Other events today
            }
        } else {
            display.setTextColor(INKPLATE_ORANGE);     // Events not today
        }

        display.setTextSize(1);
        display.setFont(&FreeMonoBoldOblique12pt7b);
        display.setCursor(10, cursorY);
        display.print(event.calendarName + ": " + truncateTitle(event.message));

        cursorY += 20;
        display.setCursor(10, cursorY);
        display.print(formatTime(event.startTime) + " - " + formatTime(event.endTime));

        cursorY += 30;  // Adjust spacing between events
    }

    // Set the font for the status message
    display.setFont(&FreeMonoBold24pt7b);
    display.setTextSize(4);
    display.setCursor(50, cursorY + 20);

    // Display BUSY!, FREE, or VERY BUSY!! based on ongoing events
    if (ongoingEventsCount == 1) {
        display.setTextColor(INKPLATE_GREEN);
        display.setCursor(75, cursorY + 160);
        display.print("FREE");
    } else if (ongoingEventsCount == 0) {
        display.setTextColor(INKPLATE_ORANGE);
        display.setCursor(60, cursorY + 160);
        display.print("BUSY!");
    } else {
        display.setTextColor(INKPLATE_RED);
        display.setCursor(10, cursorY + 20);
        display.print("VERY BUSY!!");
    }

    // Reset to default font for other text
    display.setFont(NULL);

    // Display last refresh time in small lettering on the lower left
    display.setTextSize(1);
    display.setTextColor(INKPLATE_BLACK);
    display.setCursor(10, display.height() - 20);
    lastRefreshTime = formatTime(currentTime);
    display.print("Last Refresh: " + lastRefreshTime);

    display.display();
}

// Setup function
void setup() {
    Serial.begin(115200);
    display.begin();

    if (!reconnectWiFi()) {
        displayError("WiFi Error");
        Serial.println("Failed to reconnect WiFi");
        return;
    } else {
        Serial.println("WiFi connected successfully");
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());
    }

    setRTCFromNTP();  // Update RTC from NTP

    displayCalendarEvents();  // Display calendar events

    esp_sleep_enable_timer_wakeup(15 * 60 * 1000000);  // 15-minute sleep
    esp_deep_sleep_start();
}

// Loop function (empty as the board sleeps)
void loop() {
    // Empty, as the ESP32 sleeps after setup
}