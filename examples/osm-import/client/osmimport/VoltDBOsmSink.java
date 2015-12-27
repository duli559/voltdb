package osmimport;

import java.io.IOException;
import java.net.UnknownHostException;
import java.sql.SQLException;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import java.util.Map;

import org.openstreetmap.osmosis.core.container.v0_6.BoundContainer;
import org.openstreetmap.osmosis.core.container.v0_6.EntityContainer;
import org.openstreetmap.osmosis.core.container.v0_6.EntityProcessor;
import org.openstreetmap.osmosis.core.container.v0_6.NodeContainer;
import org.openstreetmap.osmosis.core.container.v0_6.RelationContainer;
import org.openstreetmap.osmosis.core.container.v0_6.WayContainer;
import org.openstreetmap.osmosis.core.domain.v0_6.Node;
import org.openstreetmap.osmosis.core.domain.v0_6.Relation;
import org.openstreetmap.osmosis.core.domain.v0_6.RelationMember;
import org.openstreetmap.osmosis.core.domain.v0_6.Tag;
import org.openstreetmap.osmosis.core.domain.v0_6.Way;
import org.openstreetmap.osmosis.core.domain.v0_6.WayNode;
import org.openstreetmap.osmosis.core.task.v0_6.Sink;
import org.openstreetmap.osmosis.pgsimple.common.NodeLocationStoreType;
import org.postgis.LineString;
import org.postgis.Polygon;
import org.voltdb.VoltProcedure;
import org.voltdb.client.Client;
import org.voltdb.client.ClientConfig;
import org.voltdb.client.ClientFactory;
import org.voltdb.client.ClientResponse;
import org.voltdb.client.ClientStatsContext;
import org.voltdb.client.ClientStatusListenerExt;
import org.voltdb.client.NoConnectionsException;
import org.voltdb.client.ProcedureCallback;
import org.voltdb.types.TimestampType;

//import SyncBenchmark.StatusListener;

public class VoltDBOsmSink extends VoltProcedure implements Sink, EntityProcessor  {

	public static final String INS_NODE_PROC = "insertNodes";
	public static final String INS_NODE_TAG_PROC = "insertNodeTags";
	public static final String INS_RELATIONS_PROC = "insertRelations";
	public static final String INS_RELATIONS_MEMBER_PROC = "insertRelationsMembers";

	public static final String INS_RELATION_TAGS_PROC = "insertRelationTags";
	public static final String INS_USERS_PROC = "insertUsers";
	public static final String INS_WAYS_PROC = "insertWays";
	public static final String INS_WAYS_NODES_PROC = "insertWaysNodes";
	public static final String INS_WAY_TAGS_PROC = "insertWayTags";
	
	private boolean enableLinestringBuilder = true;
	private boolean enableBboxBuilder = true;
	private boolean keepInvalidWays = false;
	private WayPolygonGeometryBuilder wayGeometryBuilder;

	// Reference to the database connection we will use
	private String server;
	private ClientStatsContext periodicStatsContext;
	private ClientStatsContext fullStatsContext;
	
	private Client client;
	
	public VoltDBOsmSink(String server) {
		this.server = server;
		NodeLocationStoreType storeType = NodeLocationStoreType.TempFile;
		wayGeometryBuilder = new WayPolygonGeometryBuilder(storeType);
	}

	@Override
	public void initialize(Map<String, Object> arg0) {
		try {
			connect(server);
		} catch (Exception e) {
			e.printStackTrace();
			System.exit(1);
		}
		
	}

	private void connect(String servers) throws InterruptedException, ClassNotFoundException, SQLException {
		System.out.println("Connecting to VoltDB...");

		ClientConfig clientConfig = new ClientConfig("", "", new VoltDBOsmSink.StatusListener());
        clientConfig.setMaxTransactionsPerSecond(10000);

        client = ClientFactory.createClient(clientConfig);
        
        try {
        	// if we have more then one server, we would connect to each one individually inside a loop.
			client.createConnection(servers);
		} catch (UnknownHostException e) {
			e.printStackTrace();
		} catch (IOException e) {
			e.printStackTrace();
		}
        
		periodicStatsContext = client.createStatsContext();
		fullStatsContext = client.createStatsContext();

		
	}

	@Override
	public void complete() {
		
	}

	@Override
	public void release() {
		try {
			client.drain();
		} catch (NoConnectionsException | InterruptedException e) {
			e.printStackTrace();
		}
		
		try {
			client.close();
		} catch (InterruptedException e) {
			e.printStackTrace();
		}

	}

	@Override
	public void process(EntityContainer entity) {
		entity.process(this);
	}

	public void process(BoundContainer boundContainer) {
		// Do nothing.
	}

	public void process(NodeContainer nodeContainer)  {
		Node node;

		node = nodeContainer.getEntity();
		double lat = node.getLatitude();
		double lng = node.getLongitude();
		String pointText = "POINT(" + lng + " " + lat + ")";

		

		// keep track of the nodes so we can build polygons later
		if (enableBboxBuilder || enableLinestringBuilder) {
			wayGeometryBuilder.addNodeLocation(node);
		}
		

			//client.callProcedure(callback, procName, parameters)
			
			try {
				client.callProcedure(new InsertCallback(), INS_NODE_PROC, node.getId(),node.getVersion(),node.getUser().getId(),new TimestampType(node.getTimestamp().getTime()),node.getChangesetId(),pointText);
			} catch (NoConnectionsException e) {
				e.printStackTrace();
			} catch (IOException e) {
				e.printStackTrace();
			}
			
		
		
		Collection<Tag> tags = node.getTags();
		for ( Tag tag : tags ) {
				
				//System.out.println(INS_NODE_TAG_PROC+","+node.getId()+","+tag.getKey()+","+tag.getValue());
				try {
					client.callProcedure(new InsertCallback(), INS_NODE_TAG_PROC, node.getId(),tag.getKey(),tag.getValue());
				} catch (NoConnectionsException e) {
					e.printStackTrace();
				} catch (IOException e) {
					e.printStackTrace();
				}
		}
		
	}

	public void process(WayContainer wayContainer) {
		Way way;
		List<Long> nodeIds;

		way = wayContainer.getEntity();

		nodeIds = new ArrayList<Long>(way.getWayNodes().size());
		
		for (WayNode wayNode : way.getWayNodes()) {
			nodeIds.add(wayNode.getNodeId());
		}

		// Keep invalid ways out of the database if desired by the user
		if (way.getWayNodes().size() > 1 || keepInvalidWays) {
			
			for (Tag tag : way.getTags()) {
				try {
					client.callProcedure(new InsertCallback(), INS_WAY_TAGS_PROC, way.getId(),tag.getKey(),tag.getValue());
				} catch (NoConnectionsException e) {
					e.printStackTrace();
				} catch (IOException e) {
					e.printStackTrace();
				}
				
				if ("area".equals(tag.getKey())) {
					System.out.print("key:" + tag.getKey() + " val:" + tag.getValue());
				}
			}
			
			// Add these to the ways_nodes_table;
			int sequence = 0;
			for (Long nodeId : nodeIds) {
				try {
					client.callProcedure(new InsertCallback(), INS_WAYS_NODES_PROC, way.getId(), nodeId, sequence);
				} catch (NoConnectionsException e) {
					e.printStackTrace();
				} catch (IOException e) {
					e.printStackTrace();
				}
				sequence++;

			}
			
			StringBuffer sb = new StringBuffer();
			
			// if the first node id == the last nodeId, we know that this is a
			// closed loop.
			long n0 = nodeIds.get(0);
			long nn = nodeIds.get(nodeIds.size() - 1);
			if (n0 == nn) {

				if (enableBboxBuilder) {
					Polygon pg = wayGeometryBuilder.createPolygon(way);
					pg.outerWKT(sb);
				}

			} else {
				// it's a lineString, but we don't support it yet.
				if (enableLinestringBuilder) {
					LineString lineString = wayGeometryBuilder.createWayLinestring(way);
					lineString.outerWKT(sb);
				}
			}
			
			String bbox = sb.toString();
			
			try {
				client.callProcedure(new InsertCallback(), INS_WAYS_PROC, way.getId(),way.getVersion(),way.getUser().getId(),way.getTimestamp(),way.getChangesetId(),bbox);
			} catch (NoConnectionsException e) {
				e.printStackTrace();
			} catch (IOException e) {
				e.printStackTrace();
			}

		}

	}

	/**
	 * {@inheritDoc}
	 */
	public void process(RelationContainer relationContainer) {

		Relation relation;
		int memberSequenceId;

		relation = relationContainer.getEntity();
		
		try {
			client.callProcedure(new InsertCallback(), INS_RELATIONS_PROC, relation.getId(),relation.getVersion(),relation.getUser().getId(),relation.getTimestamp(),relation.getChangesetId());
		} catch (NoConnectionsException e) {
			e.printStackTrace();
		} catch (IOException e) {
			e.printStackTrace();
		}
		

		memberSequenceId = 0;
		for ( RelationMember member : relation.getMembers() ) {
			try {
				client.callProcedure(new InsertCallback(), INS_RELATIONS_MEMBER_PROC, relation.getId(),member.getMemberId(),member.getMemberType().ordinal(),member.getMemberRole(),memberSequenceId);
			} catch (NoConnectionsException e) {
				e.printStackTrace();
			} catch (IOException e) {
				e.printStackTrace();
			}
			memberSequenceId++;
		}
		
		for (Tag tag : relation.getTags()) {
			try {
				client.callProcedure(new InsertCallback(), INS_RELATION_TAGS_PROC, relation.getId(), tag.getKey(),
						tag.getValue());
			} catch (NoConnectionsException e) {
				e.printStackTrace();
			} catch (IOException e) {
				e.printStackTrace();
			}
		}
	}
	
	/**
     * Provides a callback to be notified on node failure.
     * This example only logs the event.
     */
    public static class StatusListener extends ClientStatusListenerExt {
        @Override
        public void connectionLost(String hostname, int port, int connectionsLeft, DisconnectCause cause) {
                System.err.printf("Connection to %s:%d was lost.\n", hostname, port);
        }
        
        public void backpressure(boolean status) {
        	
        }
    }
    
	public static class InsertCallback implements ProcedureCallback {

		@Override
		public void clientCallback(ClientResponse response) throws Exception {

			if (response.getStatus() != ClientResponse.SUCCESS){
				System.err.println(response.getStatusString());
				return;
			}

		}
    	
    }
    

}