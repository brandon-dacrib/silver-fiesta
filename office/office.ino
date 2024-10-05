#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Inkplate.h>
#include <TimeLib.h>
#include <WiFiUdp.h>
#include <esp_sleep.h>
#include "config.h"  // Include your configuration header

#include "FreeMonoBold24pt7b.h"       // Font for status messages
#include "FreeMonoBoldOblique12pt7b.h" // Font for event titles

Inkplate display;
WiFiUDP ntpUDP;
const char* ntpServer = "pool.ntp.org";
time_t lastRefreshTimestamp;

// Function to update RTC from NTP and handle synchronization failures
bool setRTCFromNTP() {
    configTime(0, 0, ntpServer);  // Sync time without timezone adjustments
    delay(2000);  // Wait for 2 seconds to allow time to sync
    time_t nowTime = time(nullptr);
    int retryCount = 0;

    // Retry loop for fetching time, up to 15 attempts
    while (nowTime < 1609459200 && retryCount < 15) {  // Wait until the time is valid (greater than 01/01/2021)
        delay(1000);
        nowTime = time(nullptr);  // Get the current time
        retryCount++;
        Serial.print("Waiting for NTP time... Attempt: ");
        Serial.println(retryCount);
    }

    if (nowTime >= 1609459200) {
        lastRefreshTimestamp = nowTime;
        setTime(nowTime);  // Set the RTC to the correct time
        Serial.println("NTP Time Sync Successful.");
        return true;  // Time successfully fetched
    }

    Serial.println("NTP Time Sync Failed.");
    return false;  // Time sync failed after retries
}

// Use RTC time as fallback when NTP sync fails
void fallbackToRTCTime() {
    time_t rtcTime = now();  // Use the RTC's time
    if (rtcTime < 1609459200) {  // Check if RTC time is also invalid
        Serial.println("RTC Time Invalid.");
    } else {
        Serial.println("Using RTC time as fallback.");
        lastRefreshTimestamp = rtcTime;  // Set the last refresh to the current RTC time
    }
}

// WiFi reconnection
bool reconnectWiFi() {
    WiFi.begin(ssid, password);  
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 10) {
        delay(1000);
        retries++;
        Serial.println("Connecting to WiFi...");
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi Connected.");
        return true;
    } else {
        Serial.println("WiFi Connection Failed.");
        return false;
    }
}

// Parse ISO8601 date string (format: "2024-10-04 11:00:00")
// Since times are already in local time, we don't need to convert to UTC.
time_t parseDateTime(String dateTime) {
    struct tm tm;
    if (strptime(dateTime.c_str(), "%Y-%m-%d %H:%M:%S", &tm)) {
        return mktime(&tm);  // Keep the time in local time
    }
    return 0;  // Return 0 if parsing fails
}

// Fetch and parse calendar events from Home Assistant
bool fetchCalendarEvent(const char* url, String& calendarName, String& message, time_t& startTime, time_t& endTime) {
    HTTPClient http;
    http.begin(url);
    http.addHeader("Authorization", homeAssistantToken);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        DynamicJsonDocument doc(4096);  // Increased size to handle event data
        deserializeJson(doc, payload);

        // Extract the relevant fields from the response
        calendarName = doc["attributes"]["friendly_name"].as<String>();
        message = doc["attributes"]["message"].as<String>();
        String start = doc["attributes"]["start_time"].as<String>();
        String end = doc["attributes"]["end_time"].as<String>();

        // Parse start and end times (no conversion needed since they are in local time)
        startTime = parseDateTime(start);
        endTime = parseDateTime(end);

        return true;
    } else {
        Serial.println("Error fetching event.");
        return false;
    }
}

// Display the current date at the top left in blue and underline it
void displayCurrentDate() {
    display.setFont(&FreeMonoBoldOblique12pt7b);  // Set the font for the date
    display.setTextSize(2);
    display.setTextColor(INKPLATE_BLUE);  // Display the date in blue
    int dateY = 40;
    display.setCursor(10, dateY);  // Position the date at the top left
    time_t currentTime = now();

    // Check if the time is valid before printing
    if (currentTime < 1609459200) {  // If time is still invalid
        display.print("Waiting for time...");
        Serial.println("Time is not set correctly.");
    } else {
        // Format and display the local date
        struct tm *localTimeInfo = localtime(&currentTime);
        char dateBuffer[16];
        strftime(dateBuffer, sizeof(dateBuffer), "%m/%d/%Y", localTimeInfo);  // Local date format MM/DD/YYYY
        display.print(dateBuffer);
        Serial.print("Current Date: ");
        Serial.println(dateBuffer);

        // Underline the date
        int dateWidth = display.getCursorX() - 10;  // Calculate date width for underline
        display.drawLine(10, dateY + 5, 10 + dateWidth, dateY + 5, INKPLATE_BLUE);
    }
}

// Draw the bottom 1/3rd with the chosen color and expand it 10% upward
void drawBottomThird(const char* status, uint16_t backgroundColor) {
    int screenHeight = display.height();
    int screenWidth = display.width();
    
    // Fill the bottom 1/3rd of the screen and expand the top by 10%
    int boxHeight = screenHeight / 3 + (screenHeight / 10);
    int boxY = screenHeight - boxHeight;

    display.fillRect(0, boxY, screenWidth, boxHeight, backgroundColor);
    
    // Display white text for the status message starting from the bottom left and lowered by 15%
    display.setTextColor(INKPLATE_WHITE);
    display.setFont(&FreeMonoBold24pt7b);
    display.setTextSize(4);
    display.setCursor(10, screenHeight - (screenHeight / 7.5));  // Lowered by 15%
    display.print(status);
}

// Display calendar events and status (FREE, BUSY, DND)
void displayCalendarEvents() {
    display.clearDisplay();

    // Display the current date at the top in blue with an underline
    displayCurrentDate();
    
    display.setFont(&FreeMonoBoldOblique12pt7b);  // Set font for event titles
    display.setTextSize(1);  // Shrink the font size for event details
    int cursorY = 80;  // Adjusted Y position to avoid overlapping with the date
    int ongoingEventsCount = 0;
    time_t currentTime = now();  // Get the current time
    time_t nextEventStartTime = 0;
    int nextEventIndex = -1;

    // Loop through the calendar events
    for (int i = 0; i < 3; i++) {
        String calendarName, message;
        time_t startTime, endTime;

        if (fetchCalendarEvent(calendarMap[i].url, calendarName, message, startTime, endTime)) {
            // Determine the next event
            if (startTime > currentTime && (nextEventStartTime == 0 || startTime < nextEventStartTime)) {
                nextEventStartTime = startTime;
                nextEventIndex = i;
            }

            // Set the text color based on whether this is the next event or not
            if (i == nextEventIndex) {
                display.setTextColor(INKPLATE_GREEN);  // Next event in green
            } else {
                display.setTextColor(INKPLATE_BLACK);  // Other events in black
            }

            // Display the event
            display.setCursor(10, cursorY);
            display.print(String(calendarMap[i].name) + ": " + message);
            cursorY += 20;

            // Display event times (no conversion needed since they're in local time)
            display.setCursor(10, cursorY);
            display.print(formatTime(startTime) + " - " + formatTime(endTime));
            cursorY += 30;

            // Check if the event is ongoing
            if (currentTime >= startTime && currentTime <= endTime) {
                ongoingEventsCount++;
            }
        }
    }

    // Determine status based on the number of ongoing events
    if (ongoingEventsCount == 0) {
        drawBottomThird("FREE", INKPLATE_GREEN);  // No events, green background with white "FREE" text
    } else if (ongoingEventsCount == 1) {
        drawBottomThird("BUSY", INKPLATE_ORANGE);  // One event, orange background with white "BUSY" text
    } else {
        drawBottomThird("DND", INKPLATE_RED);  // Two or more events, red background with white "DND" text
    }

    // Display the last refresh time in black text at the very bottom
    display.setFont(NULL);
    display.setTextSize(1);
    display.setTextColor(INKPLATE_BLACK);
    display.setCursor(10, display.height() - 20);
    display.print("Last Refresh: " + formatTime(lastRefreshTimestamp));

    display.display();
}

// Format time to HH:MM AM/PM for display
String formatTime(time_t time) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d %s", (hour(time) % 12) ? hour(time) % 12 : 12, minute(time), (hour(time) >= 12) ? "PM" : "AM");
    return String(buf);
}

// Setup function
void setup() {
    Serial.begin(115200);  // Make sure baud rate matches your serial monitor
    display.begin();

    if (!reconnectWiFi()) {
        display.clearDisplay();
        display.setCursor(10, 50);
        display.setFont(&FreeMonoBoldOblique12pt7b);
        display.print("WiFi Error");
        display.display();
        fallbackToRTCTime();  // Use RTC time if WiFi fails
    } else {
        if (!setRTCFromNTP()) {
            display.clearDisplay();
            display.setCursor(10, 50);
            display.setFont(&FreeMonoBoldOblique12pt7b);
            display.print("NTP Time Sync Failed");
            display.display();
            fallbackToRTCTime();  // Use RTC time if NTP fails
        }
    }

    displayCalendarEvents();

    // Sleep for 15 minutes
    esp_sleep_enable_timer_wakeup(15 * 60 * 1000000);  // 15 minutes
    esp_deep_sleep_start();
}

// Empty loop (device sleeps)
void loop() {}