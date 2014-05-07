/*jslint indent: 2, nomen: true, maxlen: 100, vars: true, white: true, plusplus: true */
/*global require, window, exports, Backbone, EJS, $, templateEngine, arangoHelper*/

(function() {
  "use strict";

  window.CollectionListItemView = Backbone.View.extend({

    tagName: "div",
    className: "tile",
    template: templateEngine.createTemplate("collectionsItemView.ejs"),

    initialize: function () {
      var self = this;
      this.collectionsView = this.options.collectionsView;
    },
    events: {
      'click .iconSet.icon_arangodb_settings2': 'createEditPropertiesModal',
      'click .pull-left' : 'noop',
      'click .icon_arangodb_settings2' : 'editProperties',
//      'click #editCollection' : 'editProperties',
      'click .spanInfo' : 'showProperties',
      'click': 'selectCollection'
    },

    render: function () {
      $(this.el).html(this.template.render({
        model: this.model
      }));
      $(this.el).attr('id', 'collection_' + this.model.get('name'));
      return this;
    },

    editProperties: function (event) {
      event.stopPropagation();
      this.createEditPropertiesModal();
    },

    showProperties: function(event) {
      event.stopPropagation();
      this.createInfoModal();
/*
      window.App.navigate(
        "collectionInfo/" + encodeURIComponent(this.model.get("id")), {trigger: true}
      );
*/
    },
    
    selectCollection: function() {
      window.App.navigate(
        "collection/" + encodeURIComponent(this.model.get("name")) + "/documents/1", {trigger: true}
      );
    },
    
    noop: function(event) {
      event.stopPropagation();
    },

    unloadCollection: function () {
      this.model.unloadCollection();
      this.render();
      window.modalView.hide();
    },

    loadCollection: function () {
      this.model.loadCollection();
      this.render();
      window.modalView.hide();
    },

    deleteCollection: function () {
      this.model.destroy(
        {
          error: function() {
            arangoHelper.arangoError('Could not delete collection.');
          }
        }
      );
      this.collectionsView.render();
      window.modalView.hide();
    },

    saveModifiedCollection: function() {
      var newname;
      if (window.isCoordinator()) {
        newname = this.model.get('name');
      }
      else {
        newname = $('#change-collection-name').val();
        if (newname === '') {
          arangoHelper.arangoError('No collection name entered!');
          return 0;
        }
      }

      var collid = this.model.get('id');
      var status = this.model.get('status');

      if (status === 'loaded') {
        var result;
        if (this.model.get('name') !== newname) {
          result = this.model.renameCollection(newname);
        }

        var wfs = $('#change-collection-sync').val();
        var journalSize;
        try {
          journalSize = JSON.parse($('#change-collection-size').val() * 1024 * 1024);
        }
        catch (e) {
          arangoHelper.arangoError('Please enter a valid number');
          return 0;
        }
        var changeResult = this.model.changeCollection(wfs, journalSize);

        if (result !== true) {
          if (result !== undefined) {
            arangoHelper.arangoError("Collection error: " + result);
            return 0;
          }
        }

        if (changeResult !== true) {
          arangoHelper.arangoNotification("Collection error", changeResult);
          return 0;
        }

        if (changeResult === true) {
          this.collectionsView.render();
          window.modalView.hide();
        }
      }
      else if (status === 'unloaded') {
        if (this.model.get('name') !== newname) {
          var result2 = this.model.renameCollection(newname);
          if (result2 === true) {
            this.collectionsView.render();
            window.modalView.hide();
          }
          else {
            arangoHelper.arangoError("Collection error: " + result2);
          }
        }
        else {
          window.modalView.hide();
        }
      }
    },



    //modal dialogs

    createEditPropertiesModal: function() {

      var collectionIsLoaded = false;

      if (this.model.get('status') === "loaded") {
        collectionIsLoaded = true;
      }

      var buttons = [],
        tableContent = [];

      if (! window.isCoordinator()) {
        tableContent.push(
          window.modalView.createTextEntry(
            "change-collection-name", "Name", this.model.get('name'), false, "", true
          )
        );
      }

      if (collectionIsLoaded) {
        // needs to be refactored. move getProperties into model
        var journalSize = this.model.getProperties().journalSize;
        journalSize = journalSize/(1024*1024);

        tableContent.push(
          window.modalView.createTextEntry(
            "change-collection-size",
            "Journal size",
            journalSize,
            "The maximal size of a journal or datafile (in MB). Must be at least 1.",
            "",
            true
          )
        );
      }


      if(collectionIsLoaded) {
        // prevent "unexpected sync method error"
        /*jslint stupid: true */
        var wfs = this.model.getProperties().waitForSync;
        /*jslint stupid: false */

        tableContent.push(
          window.modalView.createSelectEntry(
            "change-collection-sync",
            "Wait for sync",
            wfs,
            "Synchronise to disk before returning from a create or update of a document.",
            [{value: false, label: "No"}, {value: true, label: "Yes"}]        )
        );
      }


      tableContent.push(
        window.modalView.createReadOnlyEntry(
          "change-collection-id", "ID", this.model.get('id'), ""
        )
      );
      tableContent.push(
        window.modalView.createReadOnlyEntry(
          "change-collection-type", "Type", this.model.get('type'), ""
        )
      );
      tableContent.push(
        window.modalView.createReadOnlyEntry(
          "change-collection-status", "Status", this.model.get('status'), ""
        )
      );
      if(collectionIsLoaded) {
        buttons.push(
          window.modalView.createNotificationButton(
            "Unload",
            this.unloadCollection.bind(this)
          )
        );
      } else {
        buttons.push(
          window.modalView.createNotificationButton(
            "Load",
            this.loadCollection.bind(this)
          )
        );
      }

      buttons.push(
        window.modalView.createDeleteButton(
          "Delete",
          this.deleteCollection.bind(this)
        )
      );
      buttons.push(
        window.modalView.createSuccessButton(
          "Save",
          this.saveModifiedCollection.bind(this)
        )
      );

      window.modalView.show(
        "modalTable.ejs",
        "Modify Collection",
        buttons,
        tableContent
      );
    },

    createInfoModal: function() {
      var buttons = [],
        tableContent = this.model;
      window.modalView.show(
        "modalCollectionInfo.ejs",
        "Collection: " + this.model.get('name'),
        buttons,
        tableContent
      );

    }

  });
}());
