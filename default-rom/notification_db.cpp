// vim: noai:ts=2:sw=2
#include "notification_db.h"
#include "url-encode.h"

unsigned long lastNotificationPull = 0;

void pullNotifications() {
  Serial.println("pullNotifications()");
  lastNotificationPull = millis();

  ChannelIterator channelIterator;
  while(channelIterator.next()){
    Serial.print("Fetch data for channel: ");
    Serial.println(channelIterator.filename(""));

    String host;
    String url;

    {
      File url_file = channelIterator.file("url", "r");
      bool read_host = true;
      while (url_file.available()) {
        char r = char(url_file.read());
        if (r == '\n') {
          read_host = false;
        } else {
          if (read_host) {
            host += r;
          } else {
            url += r;
          }
        }
      }
    }
    Serial.print("host: ");
    Serial.println(host);
    Serial.print("url: ");
    Serial.println(url);

    String fingerprint;
    {
      File fingerprint_file = channelIterator.file("fingerprint", "r");

      while (fingerprint_file.available()) {
        char r = char(fingerprint_file.read());
        if (r == '\n') break;
        fingerprint += r;
      }
    }

    Serial.print("Expected fingerprint: ");
    Serial.println(fingerprint);

    WiFiClientSecure client;
    Serial.print("connecting to ");
    Serial.println(host);
    if (!client.connect(host.c_str(), 443)) {
      Serial.println("connection failed");
      continue;
    }

    if (client.verify(fingerprint.c_str(), host.c_str())) {
      Serial.println("fingerprint matches");
    } else {
      Serial.println("fingerprint doesn't match");
      continue;
    }

    Serial.print("requesting URL: ");
    Serial.println(url);

    client.print(String("GET ") + url + " HTTP/1.1\r\n" +
        "Host: " + host + "\r\n" +
        "User-Agent: BuildFailureDetectorESP8266\r\n" +
        "Connection: close\r\n\r\n");

    Serial.println("request sent");
    while (client.connected()) {
      String line = client.readStringUntil('\n');
      if (line == "\r") {
        Serial.println("headers received");
        break;
      }
    }

    {
      String timestamp_str = client.readStringUntil('\n');
      String local_timestamp_str(millis() / 1000);
      Serial.print("server timestamp: ");
      Serial.println(timestamp_str);
      Serial.print("local timestamp: ");
      Serial.println(local_timestamp_str);
      File timestamp_file = channelIterator.file("timestamp", "w");
      timestamp_file.println(timestamp_str);
      timestamp_file.println(local_timestamp_str);
    }

    {
      File data_file = channelIterator.file("data", "w");
      while (client.available()) {
        uint8_t buf[128];
        int n = client.read(buf, 128);
        data_file.write(buf, n);
        Serial.write(buf, n);
      }
    }
    Serial.println("Data written to file");
  }

  syncStatesWithData();
  recalculateStates();

}

void syncStatesWithData() {
  ChannelIterator channelIterator;

  while(channelIterator.next()){
    {
      //Add new notifications to states
      //Iterate data - find in states
      //If found: copy to new states file
      //Delete old states file
      File data_file = channelIterator.file("data", "r");
      File new_states_file = channelIterator.file("new_states", "w");
      while (data_file.available()) {
        size_t data_file_position = data_file.position();
        String line = data_file.readStringUntil('\n');
        UrlDecode decoder(line.c_str());
        char * id_str = decoder.getKey("id");
        char * valid_from_str = decoder.getKey("valid_from");
        char * valid_to_str = decoder.getKey("valid_to");
        Serial.print("data line id: ");
        Serial.println(id_str);
        if (id_str) {
          int data_line_id = atoi(id_str);
          uint32_t valid_from = atol(valid_from_str);
          uint32_t valid_to = atol(valid_to_str);
          delete id_str;
          delete valid_from_str;
          delete valid_to_str;

          File states_file = channelIterator.file("states", "r");
          NotificationStateIterator notificationStateIterator(states_file);
          bool found = false;
          while (notificationStateIterator.next()) {
            NotificationStateEntry entry = notificationStateIterator.get();
            if (data_line_id == entry.id) {
              Serial.println("Found id in states");
              entry.valid_from = valid_from;
              entry.valid_to = valid_to;
              entry.data_file_position = data_file_position;
              new_states_file.write(reinterpret_cast<uint8_t*>(&entry), sizeof(NotificationStateEntry));
              found = true;
              break;
            }
          }
          if (!found) {
            //Not found in states file
            Serial.println("Did not find id in states");
            NotificationStateEntry entry;
            entry.id = data_line_id;
            entry.state = NotificationState::SCHEDULED;
            entry.valid_from = valid_from;
            entry.valid_to = valid_to;
            entry.data_file_position = data_file_position;
            new_states_file.write(reinterpret_cast<uint8_t*>(&entry), sizeof(NotificationStateEntry));
          }
        } else {
          Serial.println("data line without id:");
          Serial.println(line);
        }

      }
    }
    SPIFFS.remove(channelIterator.filename("states"));
    SPIFFS.rename(channelIterator.filename("new_states"), channelIterator.filename("states"));
    {
      Serial.println("states file:");
      File states_file = channelIterator.file("states", "r");
      NotificationStateIterator notificationStateIterator(states_file);
      while (notificationStateIterator.next()) {
        NotificationStateEntry entry = notificationStateIterator.get();
        Serial.print("id: ");
        Serial.print(entry.id);
        Serial.print(", state: ");
        Serial.println(int(entry.state));
      }
    }

  }
}

void deleteTimestampFiles() {
  Serial.println("deleteTimestampFiles()");
  Dir dir = SPIFFS.openDir("/notif/chan");
  while(dir.next()){
    File entry = dir.openFile("r");
    String channelDir(entry.name());
    entry.close();
    if (channelDir.endsWith("/timestamp")) {
      Serial.print("removing: ");
      Serial.println(channelDir);
      SPIFFS.remove(channelDir);
    }
  }
}

void recalculateStates() {
  Serial.println("recalculateStates()");
  ChannelIterator channelIterator;

  while(channelIterator.next()){
    uint32_t current_server_timestamp = 0;
    {
      File timestamp_file = channelIterator.file("timestamp", "r");
      String server_timestamp_str = timestamp_file.readStringUntil('\n');
      String local_timestamp_str = timestamp_file.readStringUntil('\n');
      uint32_t server_timestamp = atol(server_timestamp_str.c_str());
      uint32_t local_timestamp = atol(local_timestamp_str.c_str());
      current_server_timestamp = (millis()/1000 - local_timestamp) + server_timestamp;
      Serial.print("calculated server ts: ");
      Serial.println(current_server_timestamp);
    }
    {
      File states_file = channelIterator.file("states", "r+");
      NotificationStateIterator notificationStateIterator(states_file);
      while (notificationStateIterator.next()) {
        NotificationStateEntry entry = notificationStateIterator.get();
        entry.updateState(current_server_timestamp);
        notificationStateIterator.update(entry);
      }
      Serial.println("done recalulating");
    } 
    {
      Serial.println("recalculated states file:");
      File states_file = channelIterator.file("states", "r");
      NotificationStateIterator notificationStateIterator(states_file);
      while (notificationStateIterator.next()) {
        NotificationStateEntry entry = notificationStateIterator.get();
        Serial.print("id: ");
        Serial.print(entry.id);
        Serial.print(", from: ");
        Serial.print(entry.valid_from);
        Serial.print(", to: ");
        Serial.print(entry.valid_to);
        Serial.print(", state: ");
        Serial.println(int(entry.state));
      }
    }
  }
}

void NotificationStateEntry::updateState(uint32_t server_timestamp) {
  if (state == NotificationState::SCHEDULED && server_timestamp > valid_from) {
    state = NotificationState::ACTIVE_NOT_NOTIFIED;
    Serial.println("new state act");
  }
  if (server_timestamp > valid_to) {
    state = NotificationState::INACTIVE;
    Serial.println("new state inact");
  }
}

NotificationStateIterator::NotificationStateIterator(File state_file)
  : state_file(state_file) {

}

bool NotificationStateIterator::next() {
  if (state_file.available()) {
    currentFilePosition = state_file.position();
    uint8_t entry_bytes[sizeof(NotificationStateEntry)];
    if (state_file.read(entry_bytes, sizeof(NotificationStateEntry)) != sizeof(NotificationStateEntry)) {
      Serial.println("Read wrong amount of bytes");
      return false;
    }
    NotificationStateEntry * entry = reinterpret_cast<NotificationStateEntry*>(entry_bytes);
    current = *entry;
    return true;
  }
  return false;
}

void NotificationStateIterator::update(NotificationStateEntry entry) {
  state_file.seek(currentFilePosition, SeekSet);
  state_file.write(reinterpret_cast<uint8_t*>(&entry), sizeof(NotificationStateEntry));
}

NotificationStateEntry NotificationStateIterator::get() {
  return current;
}

ChannelIterator::ChannelIterator() {
  channels_dir = SPIFFS.openDir("/notif/chan");
}

bool ChannelIterator::next() {
  while(channels_dir.next()){
    File entry = channels_dir.openFile("r");
    String channelDir(entry.name());
    entry.close();
    if (channelDir.endsWith("/url")) {
      channel_dir_base = channelDir.substring(0, channelDir.length() - 3);
      return true;
    }
  }
  return false;
}

File ChannelIterator::file(const char * name, const char * mode) {
  return SPIFFS.open(filename(name), mode);
}

String ChannelIterator::filename(const char * name) {
  return channel_dir_base + name;
}

NotificationIterator::NotificationIterator(NotificationFilter filter)
  : filter(filter), notificationStateIterator(File()) {
    nextStatesFile();
}

bool NotificationIterator::nextStatesFile() {
  if (channels.next()) {
    Serial.print("NI: next state file: ");
    Serial.println(channels.filename("states"));
    File state_file = channels.file("states", "r");
    notificationStateIterator = NotificationStateIterator(state_file);
    return true;
  }
  Serial.println("NI: reached end");
  return false;
}

bool NotificationIterator::next() {
  while (true) {
    if(!notificationStateIterator.next()) {
      if (nextStatesFile()) {
        continue;
      } else {
        return false;
      }
    }
    NotificationStateEntry entry = notificationStateIterator.get();
    NotificationState state = entry.state;

    switch(filter) {
      case NotificationFilter::ALL:
        break;
      case NotificationFilter::ACTIVE:
        if (!(state >= NotificationState::ACTIVE_NOT_NOTIFIED && state < NotificationState::INACTIVE)) {
          continue;
        }
        break;
      case NotificationFilter::NOT_NOTIFIED:
        if (!(state == NotificationState::ACTIVE_NOT_NOTIFIED)) {
          continue;
        }
        break;
      default:
        Serial.println("FAIL: Undefined filter");
        return false;
    }

    current = entry;

    return true;
  }
  
}

void NotificationIterator::setCurrentNotificationState(NotificationState state) {
  current.state = state;
  notificationStateIterator.update(current);
}

NotificationStateEntry NotificationIterator::getStateEntry() {
  return current;
}

Notification NotificationIterator::getNotification() {
  File data_file = channels.file("data", "r");
  data_file.seek(current.data_file_position, SeekSet);
  String line = data_file.readStringUntil('\n');
  UrlDecode decoder(line.c_str());
  Notification noti;

  char * id_str = decoder.getKey("id");
  noti.id = atoi(id_str);
  delete id_str;

  char * summary_str = decoder.getKey("summary");
  noti.summary = String(summary_str);
  delete summary_str;

  char * description_str = decoder.getKey("description");
  noti.description = String(description_str);
  delete description_str;

  char * location_str = decoder.getKey("location");
  noti.location = String(location_str);
  delete location_str;

  return noti;
}