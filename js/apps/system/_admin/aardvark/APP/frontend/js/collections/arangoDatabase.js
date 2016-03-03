/*jshint browser: true */
/*global window, arangoHelper, Backbone, $, window, _*/

(function() {
  'use strict';

  window.ArangoDatabase = Backbone.Collection.extend({

    model: window.DatabaseModel,

    sortOptions: {
      desc: false
    },

    url: "/_api/database",

    comparator: function(item, item2) {
      var a = item.get('name').toLowerCase();
      var b = item2.get('name').toLowerCase();
      if (this.sortOptions.desc === true) {
        return a < b ? 1 : a > b ? -1 : 0;
      }
      return a > b ? 1 : a < b ? -1 : 0;
    },

    parse: function(response) {
      if (!response) {
        return;
      }
      return _.map(response.result, function(v) {
        return {name:v};
      });
    },

    initialize: function() {
      var self = this;
      this.fetch().done(function() {
        self.sort();
      });
    },

    setSortingDesc: function(yesno) {
      this.sortOptions.desc = yesno;
    },

    getDatabases: function() {
      var self = this;
      this.fetch().done(function() {
        self.sort();
      });
      return this.models;
    },

    getDatabasesForUser: function(callback) {
      $.ajax({
        type: "GET",
        cache: false,
        url: this.url + "/user",
        contentType: "application/json",
        processData: false,
        success: function(data) {
          callback(false, (data.result).sort());
        },
        error: function() {
          callback(true, []);
        }
      });
    },

    createDatabaseURL: function(name, protocol, port) {
      var loc = window.location;
      var hash = window.location.hash;
      if (protocol) {
        if (protocol === "SSL" || protocol === "https:") {
          protocol = "https:";
        } else {
          protocol = "http:";
        }
      } else {
        protocol = loc.protocol;
      }
      port = port || loc.port;

      var url = protocol
        + "//"
        + window.location.hostname
        + ":"
        + port
        + "/_db/"
        + encodeURIComponent(name)
        + "/_admin/aardvark/standalone.html";
      if (hash) {
        var base = hash.split("/")[0];
        if (base.indexOf("#collection") === 0) {
          base = "#collections";
        }
        if (base.indexOf("#application") === 0) {
          base = "#applications";
        }
        url += base;
      }
      return url;
    },

    getCurrentDatabase: function(callback) {
      $.ajax({
        type: "GET",
        cache: false,
        url: this.url + "/current",
        contentType: "application/json",
        processData: false,
        success: function(data) {
          if (data.code === 200) {
            callback(false, data.result.name);
          }
          else {
            callback(false, data);
          }
        },
        error: function(data) {
          callback(true, data);
        }
      });
    },

    hasSystemAccess: function(callback) {

      var callback2 = function(error, list) {
        if (error) {
          arangoHelper.arangoError("DB","Could not fetch databases");
        }
        else {
          callback(false, _.contains(list, "_system"));
        }
      }.bind(this);

      this.getDatabasesForUser(callback2);
    }
  });
}());
